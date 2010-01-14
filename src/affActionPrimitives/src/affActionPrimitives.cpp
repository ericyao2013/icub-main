
#include <ace/Auto_Event.h>
#include <yarp/math/Math.h>
#include <yarp/os/Time.h>

#include <gsl/gsl_math.h>

#include <iCub/ctrlMath.h>
#include <iCub/affActionPrimitives.h>

#include <stdio.h>
#include <string>

#define RES_EVENT(x)                        (static_cast<ACE_Auto_Event*>(x))
                                            
#define ACTIONPRIM_DEFAULT_PER              50      // [ms]
#define ACTIONPRIM_DEFAULT_EXECTIME         2.0     // [s]
#define ACTIONPRIM_DEFAULT_REACHTOL         0.005   // [m]
#define ACTIONPRIM_DUMP_PERIOD              1.0     // [s]
#define ACTIONPRIM_DEFAULT_PART             "right_arm"
#define ACTIONPRIM_DEFAULT_TRACKINGMODE     "off"
#define ACTIONPRIM_DEFAULT_VERBOSITY        "off"

using namespace std;
using namespace yarp;
using namespace yarp::os;
using namespace yarp::dev;
using namespace yarp::sig;
using namespace yarp::math;
using namespace ctrl;


/************************************************************************/
affActionPrimitives::affActionPrimitives() :
                     RateThread(ACTIONPRIM_DEFAULT_PER)
{
    init();
}


/************************************************************************/
affActionPrimitives::affActionPrimitives(Property &opt) :
                     RateThread(ACTIONPRIM_DEFAULT_PER)
{
    init();
    open(opt);
}


/************************************************************************/
void affActionPrimitives::init()
{
    polyHand=polyCart=NULL;

    mutex=NULL;
    motionDoneEvent=NULL;

    actionClb=NULL;

    armMoveDone =latchArmMoveDone =true;
    handMoveDone=latchHandMoveDone=true;
    configured=closed=false;
    checkEnabled=true;
    torsoActive=true;

    latchTimer=waitTmo=0.0;
}


/************************************************************************/
bool affActionPrimitives::isValid()
{
    return configured;
}

/************************************************************************/
string affActionPrimitives::toCompactString(const Vector &v)
{
    char buf[255];
    string ret;
    int i;

    for (i=0; i<v.length()-1; i++)
    {
        sprintf(buf,"%g ",v[i]);
        ret+=buf;
    }

    sprintf(buf,"%g",v[i]);
    ret+=buf;

    return ret;
}


/************************************************************************/
int affActionPrimitives::printMessage(const char *format, ...)
{
    if (verbose)
    {
        fprintf(stdout,"*** %s: ",(local+"/"+part).c_str());
    
        va_list ap;
        va_start(ap,format);    
        int ret=vfprintf(stdout,format,ap);
        va_end(ap);
        
        return ret;
    }
    else
        return -1;
}


/************************************************************************/
bool affActionPrimitives::handleTorsoDOF(Property &opt, const string &key,
                                         const int j)
{
    if (opt.check(key.c_str()))
    {
        bool sw=opt.find(key.c_str()).asString()=="on"?true:false;

        Vector newDof, dummyRet;
        newDof.resize(3,2);
        newDof[j]=sw?1:0;

        printMessage("%s %s\n",key.c_str(),sw?"enabled":"disabled");
        cartCtrl->setDOF(newDof,dummyRet);

        if (sw)
        {
            string minKey=key+"_min";
            string maxKey=key+"_max";
            double min, max;

            cartCtrl->getLimits(j,&min,&max);

            min=opt.check(minKey.c_str(),Value(min)).asDouble();
            max=opt.check(maxKey.c_str(),Value(max)).asDouble();

            cartCtrl->setLimits(j,min,max);
            cartCtrl->getLimits(j,&min,&max);

            printMessage("%s limits: [%g,%g] deg\n",key.c_str(),min,max);            
        }

        return true;
    }

    return false;
}


/************************************************************************/
bool affActionPrimitives::configHandSeq(Property &opt)
{
    if (opt.check("hand_sequences_file"))
    {
        string handSeqFile=opt.find("hand_sequences_file").asString().c_str();
        Property handSeqProp;

        printMessage("Processing %s file\n",handSeqFile.c_str());
        handSeqProp.fromConfigFile(handSeqFile.c_str());
    
        // GENERAL group
        Bottle &bGeneral=handSeqProp.findGroup("GENERAL");
        if (bGeneral.isNull())
        {
            printMessage("WARNING: \"GENERAL\" group is missing\n");    
            return false;
        }

        if (!bGeneral.check("numSequences"))
        {
            printMessage("WARNING: \"numSequences\" option is missing\n");    
            return false;
        }

        int numSequences=bGeneral.find("numSequences").asInt();

        // SEQUENCE groups
        for (int i=0; i<numSequences; i++)
        {
            char seq[255];
            sprintf(seq,"SEQ_%d",i);

            Bottle &bSeq=handSeqProp.findGroup(seq);
            if (bSeq.isNull())
            {
                printMessage("WARNING: \"%s\" group is missing\n",seq);    
                return false;
            }

            if (!bSeq.check("key"))
            {
                printMessage("WARNING: \"key\" option is missing\n");    
                return false;
            }

            string key=bSeq.find("key").asString().c_str();

            if (!bSeq.check("numWayPoints"))
            {
                printMessage("WARNING: \"numWayPoints\" option is missing\n");    
                return false;
            }

            int numWayPoints=bSeq.find("numWayPoints").asInt();

            for (int j=0; j<numWayPoints; j++)
            {
                char wp[255];
                sprintf(wp,"wp_%d",j);

                Bottle &bWP=bSeq.findGroup(wp);
                if (bWP.isNull())
                {
                    printMessage("WARNING: \"%s\" entry is missing\n",wp);    
                    return false;
                }

                if (!bWP.check("poss"))
                {
                    printMessage("WARNING: \"poss\" option is missing\n");
                    return false;
                }

                if (!bWP.check("vels"))
                {
                    printMessage("WARNING: \"vels\" option is missing\n");
                    return false;
                }

                if (!bWP.check("thres"))
                {
                    printMessage("WARNING: \"thres\" option is missing\n");
                    return false;
                }

                Bottle *bPoss=bWP.find("poss").asList();
                Vector poss(bPoss->size());

                for (int k=0; k<poss.length(); k++)
                    poss[k]=bPoss->get(k).asDouble();

                Bottle *bVels=bWP.find("vels").asList();
                Vector vels(bVels->size());

                for (int k=0; k<vels.length(); k++)
                    vels[k]=bVels->get(k).asDouble();

                Bottle *bThres=bWP.find("thres").asList();
                Vector thres(bThres->size());

                for (int k=0; k<thres.length(); k++)
                    thres[k]=bThres->get(k).asDouble();

                if (!addHandSeqWP(key,poss,vels,thres))
                    printMessage("WARNING: \"%s\" entry is invalid,\
                                 not added to \"%s\"\n",wp,key.c_str());
            }
        }
    
        return true;
    }
    else
        return false;
}


/************************************************************************/
bool affActionPrimitives::open(Property &opt)
{
    if (!opt.check("local"))
    {
        printMessage("ERROR: option \"local\" is missing\n");
        return false;
    }

    local=opt.find("local").asString().c_str();
    part=opt.check("part",Value(ACTIONPRIM_DEFAULT_PART)).asString().c_str();
    default_exec_time=opt.check("default_exec_time",Value(ACTIONPRIM_DEFAULT_EXECTIME)).asDouble();
    tracking_mode=opt.check("tracking_mode",Value(ACTIONPRIM_DEFAULT_TRACKINGMODE)).asString()=="on"?true:false;
    verbose=opt.check("verbosity",Value(ACTIONPRIM_DEFAULT_VERBOSITY)).asString()=="on"?true:false;

    string robot=opt.check("robot",Value("icub")).asString().c_str();    
    int period=opt.check("thread_period",Value(ACTIONPRIM_DEFAULT_PER)).asInt();    
    double reach_tol=opt.check("reach_tol",Value(ACTIONPRIM_DEFAULT_REACHTOL)).asDouble();    
    string fwslash="/";

    // get hand sequence motions (if any)
    configHandSeq(opt);

    // open the position client
    Property optPolyHand("(device remote_controlboard)");
    optPolyHand.put("remote",(fwslash+robot+fwslash+part).c_str());
    optPolyHand.put("local",(fwslash+local+fwslash+part+fwslash+"position").c_str());

    polyHand=new PolyDriver;
    if (!polyHand->open(optPolyHand))
    {
        close();
        return false;
    }

    // open the cartesian client
    Property optPolyCart("(device cartesiancontrollerclient)");
    optPolyCart.put("remote",(fwslash+robot+fwslash+"cartesianController"+fwslash+part).c_str());
    optPolyCart.put("local",(fwslash+local+fwslash+part+fwslash+"cartesian").c_str());

    polyCart=new PolyDriver;
    if (!polyCart->open(optPolyCart))
    {
        close();
        return false;
    }

    // open views
    polyHand->view(encCtrl);
    polyHand->view(posCtrl);
    polyCart->view(cartCtrl);

    // set tolerance
    cartCtrl->setInTargetTol(reach_tol);

    // set tracking mode
    setTrackingMode(tracking_mode);

    // handle torso DOF's
    handleTorsoDOF(opt,"torso_pitch",0);
    handleTorsoDOF(opt,"torso_roll",1);
    handleTorsoDOF(opt,"torso_yaw",2);

    Vector curDof;
    cartCtrl->getDOF(curDof);

    enableTorsoSw.resize(3,0);
    disableTorsoSw.resize(3,0);
    for (int i=0; i<3; i++)
        enableTorsoSw[i]=curDof[i];

    // start with torso disabled
    disableTorsoDof();    

    // open port for grasp detection
    graspDetectionPort.open((fwslash+local+fwslash+part+fwslash+"detectGrasp:i").c_str());

    jHandMin=7;                     // hand first joint
    posCtrl->getAxes(&jHandMax);    // hand last joint

    // hand joints set
    for (int j=jHandMin; j<jHandMax; j++)
        fingersJntsSet.insert(j);

    // map from hand joints to fingers
    fingers2JntsMap.insert(pair<int,int>(0,8));
    fingers2JntsMap.insert(pair<int,int>(0,9));
    fingers2JntsMap.insert(pair<int,int>(0,10));
    fingers2JntsMap.insert(pair<int,int>(1,11));
    fingers2JntsMap.insert(pair<int,int>(1,12));
    fingers2JntsMap.insert(pair<int,int>(2,13));
    fingers2JntsMap.insert(pair<int,int>(2,14));
    fingers2JntsMap.insert(pair<int,int>(3,15));
    fingers2JntsMap.insert(pair<int,int>(4,15));

    mutex=new Semaphore(1);
    motionDoneEvent=new ACE_Auto_Event;

    // start the thread with the specified period
    Time::turboBoost();
    setRate(period);
    start();

    return configured=true;
}


/************************************************************************/
void affActionPrimitives::close()
{
    if (closed)
        return;

    if (polyHand->isValid() && polyCart->isValid())
    {    
        stopControl();
        setTrackingMode(false);
    }

    if (isRunning())
    {
        printMessage("stopping thread ...\n");        
        stop();
    }

    if (polyHand!=NULL)
    {
        printMessage("closing hand driver ...\n");
        delete polyHand;
    }

    if (polyCart!=NULL)
    {
        printMessage("closing cartesian driver ...\n"); 
        delete polyCart;
    }

    if (!graspDetectionPort.isClosed())
    {    
        graspDetectionPort.interrupt();
        graspDetectionPort.close();
    }

    if (mutex!=NULL)
        delete mutex;

    if (motionDoneEvent!=NULL)
        delete RES_EVENT(motionDoneEvent);

    closed=true;

    printMessage("closing complete!\n");
}


/************************************************************************/
bool affActionPrimitives::isHandSeqEnded()
{
    // latch the current moving fingers set
    set<int> tmpSet=fingersMovingJntsSet;

    for (set<int>::iterator i=fingersMovingJntsSet.begin(); i!=fingersMovingJntsSet.end(); ++i)
    {
        bool flag;
        posCtrl->checkMotionDone(*i,&flag);

        if (flag)
            tmpSet.erase(*i);
    }

    // get data from the graspDetector
    if (Bottle *pB=graspDetectionPort.read(false))
    {
        // span over fingers
        for (int fng=0; fng<5; fng++)
        {
            double val=pB->get(fng).asDouble();
            double thres=curGraspDetectionThres[fng];

            // detect contact on the finger
            if (val>thres)
            {
                printMessage("contact detected on finger %d: (%g>%g)\n",
                             fng,val,thres);

                // take joints belonging to the finger
                pair<multimap<int,int>::iterator,multimap<int,int>::iterator> i=fingers2JntsMap.equal_range(fng);
                
                for (multimap<int,int>::iterator j=i.first; j!=i.second; ++j)
                {
                    int jnt=j->second;

                    // stop and remove if not done yet
                    if (tmpSet.find(jnt)!=tmpSet.end())
                    {
                        stopJntTraj(jnt);
                        tmpSet.erase(jnt);
                    }
                }                
            }
        }
    }

    // update the moving fingers set
    fingersMovingJntsSet=tmpSet;

    if (fingersMovingJntsSet.size())
        return false;
    else
        return true;
}


/************************************************************************/
bool affActionPrimitives::clearActionsQueue()
{
    if (configured)
    {
        mutex->wait();
        actionsQueue.clear();
        mutex->post();

        return true;
    }
    else
        return false;
}


/************************************************************************/
bool affActionPrimitives::pushAction(const bool execArm, const Vector &x, const Vector &o,
                                     const double execTime, const bool execHand,
                                     const HandWayPoint &handWP, affActionCallback *clb)
{
    if (configured)
    {
        mutex->wait();
        Action action;
    
        action.waitState=false;
        action.execArm=execArm;
        action.x=x;
        action.o=o;
        action.execTime=execTime;
        action.execHand=execHand;
        action.handWP=handWP;
        action.clb=clb;

        actionsQueue.push_back(action);
        mutex->post();

        return true;
    }
    else
        return false;
}


/************************************************************************/
bool affActionPrimitives::pushAction(const Vector &x, const Vector &o,
                                     const string &handSeqKey, const double execTime,
                                     affActionCallback *clb)
{
    if (configured)
    {
        map<string,deque<HandWayPoint> >::iterator itr=handSeqMap.find(handSeqKey);
        if (itr!=handSeqMap.end())
        {
            deque<HandWayPoint> &q=itr->second;            

            if (q.size())
            {   
                Vector dummy(1);

                // action is combined, hence it's ok to resever
                // the callback at the end of reaching part
                pushAction(true,x,o,execTime,true,q[0],clb);

                // decompose hand action in sum of fingers sequences
                for (unsigned int i=1; i<q.size(); i++)
                    pushAction(false,dummy,dummy,ACTIONPRIM_DISABLE_EXECTIME,true,q[i],NULL);
            }

            return true;
        }
        else
        {
            printMessage("WARNING: \"%s\" hand sequence key not found\n",
                         handSeqKey.c_str());    

            return false;
        }
    }
    else
        return false;
}


/************************************************************************/
bool affActionPrimitives::pushAction(const Vector &x, const Vector &o,
                                     const double execTime,
                                     affActionCallback *clb)
{
    if (configured)
    {
        HandWayPoint dummy;
        pushAction(true,x,o,execTime,false,dummy,clb);

        return true;
    }
    else
        return false;
}


/************************************************************************/
bool affActionPrimitives::pushAction(const string &handSeqKey,
                                     affActionCallback *clb)
{
    if (configured)
    {
        map<string,deque<HandWayPoint> >::iterator itr=handSeqMap.find(handSeqKey);
        if (itr!=handSeqMap.end())
        {
            deque<HandWayPoint> &q=itr->second;
            unsigned int i;
            Vector dummy(1);

            // decompose hand action in sum of fingers sequences
            for (i=0; i<q.size()-1; i++)
                pushAction(false,dummy,dummy,ACTIONPRIM_DISABLE_EXECTIME,true,q[i],NULL);

            // reserve the callback whenever the last hand WP is achieved
            if (i<q.size())
                pushAction(false,dummy,dummy,ACTIONPRIM_DISABLE_EXECTIME,true,q[i],clb);

            return true;
        }
        else
        {
            printMessage("WARNING: \"%s\" hand sequence key not found\n",
                         handSeqKey.c_str());    

            return false;
        }
    }
    else
        return false;
}


/************************************************************************/
bool affActionPrimitives::pushWaitState(const double tmo, affActionCallback *clb)
{
    if (configured)
    {
        mutex->wait();
        Action action;
        Vector dummy(1);

        action.waitState=true;
        action.tmo=tmo;
        action.execArm=false;
        action.execHand=false;
        action.clb=clb;

        actionsQueue.push_back(action);
        mutex->post();

        return true;
    }
    else
        return false;
}


/************************************************************************/
bool affActionPrimitives::reach(const Vector &x, const Vector &o,
                                const double execTime)
{
    if (configured)
    {
        const double t=execTime>0.0?execTime:default_exec_time;

        enableTorsoDof();

        cartCtrl->goToPose(x,o,t);

        latchArmMoveDone=armMoveDone=false;

        printMessage("reach at %g [s] for [%s], [%s]\n",t,
                     toCompactString(x).c_str(),
                     toCompactString(o).c_str());

        t0=Time::now();

        return true;
    }
    else
        return false;
}


/************************************************************************/
bool affActionPrimitives::execQueuedAction()
{
    bool exec=false;
    Action action;

    mutex->wait();
    if (actionsQueue.size())
    {
        action=actionsQueue.front();
        actionsQueue.pop_front();
        exec=true;
    }
    mutex->post();

    if (exec)
    {
        if (action.waitState)
            wait(action);

        if (action.execArm)
            cmdArm(action);

        if (action.execHand)
            cmdHand(action);

        actionClb=action.clb;
    }

    return exec;
}


/************************************************************************/
bool affActionPrimitives::execPendingHandSequences()
{
    bool exec=false;
    Action action;

    mutex->wait();
    if (actionsQueue.size())
    {
        // polling on the first action in the queue
        action=actionsQueue.front();

        // if it is an hand-action then execute and update queue
        if (action.execHand && !action.execArm && !action.waitState)
        {    
            actionsQueue.pop_front();
            cmdHand(action);
            exec=true;
        }
    }
    mutex->post();

    return exec;
}


/************************************************************************/
void affActionPrimitives::run()
{
    const double t=Time::now();

    if (!armMoveDone)
    {
        Vector x,o,xdcap,odcap,qdcap;
        cartCtrl->getPose(x,o);
        cartCtrl->getDesired(xdcap,odcap,qdcap);

        if (t-t0>ACTIONPRIM_DUMP_PERIOD)
        {
            printMessage("reaching... xdcap=[%s] |e|=%.3f [m]\n",
                         toCompactString(xdcap).c_str(),norm(xdcap-x));

            t0=t;
        }

        cartCtrl->checkMotionDone(&armMoveDone);

        if (armMoveDone)
        {    
            printMessage("reaching complete\n");
            disableTorsoDof();
        }
    }

    if (!handMoveDone)
    {
        // check whether all the remaining active joints have come
        // to a complete stop
        if (handMoveDone=isHandSeqEnded())
        {    
            printMessage("hand WP reached\n");
            execPendingHandSequences();    // here handMoveDone may switch false again
        }
    }

    latchArmMoveDone=armMoveDone;
    latchHandMoveDone=handMoveDone;

    if (latchArmMoveDone && latchHandMoveDone && (t-latchTimer>waitTmo))
    {
        // execute action-end callback
        if (actionClb)
        {            
            actionClb->exec();
            actionClb=NULL;
        }
    
        if (!execQueuedAction())
            RES_EVENT(motionDoneEvent)->signal();
    }
}


/************************************************************************/
affActionPrimitives::~affActionPrimitives()
{
    close();
}


/************************************************************************/
bool affActionPrimitives::stopJntTraj(const int jnt)
{
    double v;

    if (encCtrl->getEncoder(jnt,&v))
        return posCtrl->positionMove(jnt,v);
    else
        return false;
}


/************************************************************************/
void affActionPrimitives::enableTorsoDof()
{
    // enable torso joints, if any
    if (!torsoActive && norm(enableTorsoSw))
    {
        Vector dummyRet;

        cartCtrl->setDOF(enableTorsoSw,dummyRet);

        torsoActive=true;
    }
}


/************************************************************************/
void affActionPrimitives::disableTorsoDof()
{
    // disable torso joints, if any
    if (torsoActive && norm(enableTorsoSw))
    {
        Vector dummyRet;

        cartCtrl->setDOF(disableTorsoSw,dummyRet);

        torsoActive=false;
    }
}


/************************************************************************/
bool affActionPrimitives::wait(const Action &action)
{
    if (configured)
    {        
        printMessage("wait for %g seconds\n",action.tmo);
        waitTmo=action.tmo;
        latchTimer=Time::now();

        return true;
    }
    else
        return false;
}


/************************************************************************/
bool affActionPrimitives::cmdArm(const Action &action)
{
    if (configured)
    {
        const Vector &x=action.x;
        const Vector &o=action.o;
        const double t=action.execTime>0.0?action.execTime:default_exec_time;

        enableTorsoDof();

        if (!cartCtrl->goToPoseSync(x,o,t))
        {
            printMessage("reach error\n");
            return false;
        }

        latchArmMoveDone=armMoveDone=false;

        printMessage("reach at %g [s] for [%s], [%s]\n",
                     t,toCompactString(x).c_str(),
                     toCompactString(o).c_str());

        t0=Time::now();

        return true;
    }
    else
        return false;
}


/************************************************************************/
bool affActionPrimitives::cmdHand(const Action &action)
{
    if (configured)
    {        
        const string &tag=action.handWP.tag;
        const Vector &poss=action.handWP.poss;
        const Vector &vels=action.handWP.vels;
        const Vector &thres=action.handWP.thres;

        fingersMovingJntsSet=fingersJntsSet;
        curGraspDetectionThres=thres;
        for (set<int>::iterator itr=fingersJntsSet.begin(); itr!=fingersJntsSet.end(); ++itr)
        {   
            int j=*itr-jHandMin;

            if (j>=poss.length() || j>=vels.length())
                break;

            posCtrl->setRefSpeed(*itr,vels[j]);
            posCtrl->positionMove(*itr,poss[j]);
        }

        latchHandMoveDone=handMoveDone=false;
        printMessage("\"%s\" WP: [%s] (thres = [%s])\n",
                     tag.c_str(),toCompactString(poss).c_str(),
                     toCompactString(thres).c_str());

        return true;
    }
    else
        return false;
}


/************************************************************************/
bool affActionPrimitives::addHandSeqWP(const string &handSeqKey, const Vector &poss,
                                       const Vector &vels, const Vector &thres)
{
    if (poss.length()==9 && vels.length()==9 && thres.length()==5)
    {
        HandWayPoint handWP;

        handWP.tag=handSeqKey;
        handWP.poss=poss;
        handWP.vels=vels;
        handWP.thres=thres;
    
        handSeqMap[handSeqKey].push_back(handWP);

        return true;
    }
    else
        return false;
}


/************************************************************************/
bool affActionPrimitives::isValidHandSeq(const string &handSeqKey)
{
    map<string,deque<HandWayPoint> >::iterator itr=handSeqMap.find(handSeqKey);

    if (itr!=handSeqMap.end())
        return true;
    else
        return false;
}


/************************************************************************/
bool affActionPrimitives::removeHandSeq(const string &handSeqKey)
{
    map<string,deque<HandWayPoint> >::iterator itr=handSeqMap.find(handSeqKey);

    if (itr!=handSeqMap.end())
    {    
        handSeqMap[handSeqKey].clear();
        return true;
    }
    else
        return false;
}


/************************************************************************/
deque<string> affActionPrimitives::getHandSeqList()
{
    map<string,deque<HandWayPoint> >::iterator itr;
    deque<string> q;

    for (itr=handSeqMap.begin(); itr!=handSeqMap.end(); ++itr)
        q.push_back(itr->first);

    return q;
}


/************************************************************************/
bool affActionPrimitives::getPose(Vector &x, Vector &o)
{
    if (configured)
        return cartCtrl->getPose(x,o);
    else
        return false;
}


/************************************************************************/
bool affActionPrimitives::stopControl()
{
    if (configured)
    {
        suspend();

        clearActionsQueue();

        cartCtrl->stopControl();

        for (set<int>::iterator itr=fingersJntsSet.begin(); itr!=fingersJntsSet.end(); ++itr)
            stopJntTraj(*itr);

        armMoveDone =latchArmMoveDone =true;
        handMoveDone=latchHandMoveDone=true;

        resume();

        return true;
    }
    else
        return false;
}


/************************************************************************/
bool affActionPrimitives::setTrackingMode(const bool f)
{
    if (cartCtrl->setTrackingMode(f))
    {
        tracking_mode=f;
        return true;
    }
    else
        return false;
}


/************************************************************************/
bool affActionPrimitives::getTrackingMode()
{
    return tracking_mode;
}


/************************************************************************/
bool affActionPrimitives::checkActionsDone(bool &f, const bool sync)
{
    if (configured)
    {
        if (sync && checkEnabled)
        {
            RES_EVENT(motionDoneEvent)->reset();
            RES_EVENT(motionDoneEvent)->wait();
        }

        f=latchArmMoveDone && latchHandMoveDone;

        return true;
    }
    else
        return false;
}


/************************************************************************/
bool affActionPrimitives::syncCheckInterrupt(const bool disable)
{
    if (configured)
    {
        RES_EVENT(motionDoneEvent)->signal();

        if (disable)
            checkEnabled=false;

        return true;
    }
    else
        return false;
}


/************************************************************************/
bool affActionPrimitives::syncCheckReinstate()
{
    if (configured)
    {
        checkEnabled=true;
        return true;
    }
    else
        return false;
}


/************************************************************************/
bool affActionPrimitivesLayer1::grasp(const Vector &x, const Vector &o, const Vector &d)
{
    if (configured)
    {
        printMessage("start grasping\n");
        pushAction(x+d,o,"open_hand");
        pushAction(x,o);
        pushAction("close_hand");

        return true;
    }
    else
        return false;
}


/************************************************************************/
bool affActionPrimitivesLayer1::touch(const Vector &x, const Vector &o, const Vector &d)
{
    if (configured)
    {
        printMessage("start touching\n");
        pushAction(x+d,o,"open_hand");
        pushAction(x,o);

        return true;
    }
    else
        return false;
}


/************************************************************************/
bool affActionPrimitivesLayer1::tap(const Vector &x1, const Vector &o1,
                                    const Vector &x2, const Vector &o2,
                                    const double execTime)
{
    if (configured)
    {
        printMessage("start tapping\n");
        pushAction(x1,o1,"karate_hand");
        pushAction(x2,o2,execTime);
        pushAction(x1,o1);

        return true;
    }
    else
        return false;
}


/************************************************************************/
affActionPrimitivesLayer2::affActionPrimitivesLayer2() :
                           affActionPrimitivesLayer1()
{
    init();
}


/************************************************************************/
affActionPrimitivesLayer2::affActionPrimitivesLayer2(Property &opt) :
                           affActionPrimitivesLayer1(opt)
{
    init();
    skipFatherPart=true;
    open(opt);
}


/************************************************************************/
void affActionPrimitivesLayer2::init()
{
    // default values
    wrist_joint=8;
    wrist_thres=1e9;

    skipFatherPart=false;
    enableWristCheck=false;

    disableWristDof=NULL;
    enableWristDof=NULL;
}


/************************************************************************/
void affActionPrimitivesLayer2::run()
{
    if (enableWristCheck)
    {
        double out;

        if (pidCtrl->getOutput(wrist_joint,&out))
        {
            out=out<0.0?-out:out;

            if (out>wrist_thres)
            {
                printMessage("contact detected on the wrist joint %d: (%g>%g)\n",
                             wrist_joint,out,wrist_thres);

                cartCtrl->stopControl();
            }
        }
    }

    affActionPrimitivesLayer1::run();
}


/************************************************************************/
bool affActionPrimitivesLayer2::open(Property &opt)
{
    if (!configured && !skipFatherPart)
        affActionPrimitivesLayer1::open(opt);

    if (configured)
    {    
        if (Bottle *bWrist=opt.find("wrist_joint").asList())
        {
            if (bWrist->size()>1)
            {
                wrist_joint=bWrist->get(0).asInt();
                wrist_thres=bWrist->get(1).asDouble();
            }
            else
                printMessage("WARNING: invalid \"wrist_joint\" option\n");
        }

        // check wrist params
        Vector curDof;
        cartCtrl->getDOF(curDof);
        int max=curDof.length()-1;
        wrist_joint=wrist_joint<0?0:(wrist_joint>max?max:wrist_joint);
        wrist_thres=wrist_thres<0.0?0.0:wrist_thres;
        
        Vector disableWristSw;
        disableWristSw.resize(wrist_joint+1,2);
        disableWristSw[wrist_joint]=0;

        Vector enableWristSw;
        enableWristSw.resize(wrist_joint+1,2);
        enableWristSw[wrist_joint]=1;

        // create callbacks
        disableWristDof=new switchingWristDof(cartCtrl,disableWristSw,&enableWristCheck);
        enableWristDof =new switchingWristDof(cartCtrl,enableWristSw,&enableWristCheck);

        polyHand->view(pidCtrl);

        return true;
    }
    else
        return false;
}


/************************************************************************/
bool affActionPrimitivesLayer2::grasp(const Vector &x, const Vector &o, const Vector &d)
{
    if (configured)
    {
        printMessage("start grasping\n");
        pushAction(x+d,o,"open_hand",ACTIONPRIM_DISABLE_EXECTIME,disableWristDof);
        pushAction(x,o,ACTIONPRIM_DISABLE_EXECTIME,enableWristDof);
        pushAction("close_hand");

        return true;
    }
    else
        return false;
}


/************************************************************************/
bool affActionPrimitivesLayer2::touch(const Vector &x, const Vector &o, const Vector &d)
{
    if (configured)
    {
        printMessage("start touching\n");
        pushAction(x+d,o,"open_hand",ACTIONPRIM_DISABLE_EXECTIME,disableWristDof);
        pushAction(x,o,ACTIONPRIM_DISABLE_EXECTIME,enableWristDof);

        return true;
    }
    else
        return false;
}


/************************************************************************/
affActionPrimitivesLayer2::~affActionPrimitivesLayer2()
{
    if (disableWristDof)
        delete disableWristDof;

    if (enableWristDof)
        delete enableWristDof;
}


