/* 
 * Copyright (C) 2010 RobotCub Consortium, European Commission FP6 Project IST-004370
 * Author: Ugo Pattacini
 * email:  ugo.pattacini@iit.it
 * website: www.robotcub.org
 * Permission is granted to copy, distribute, and/or modify this program
 * under the terms of the GNU General Public License, version 2 or any
 * later version published by the Free Software Foundation.
 *
 * A copy of the license can be found at
 * http://www.robotcub.org/icub/license/gpl.txt
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details
*/

#ifndef __LOCALIZER_H__
#define __LOCALIZER_H__

#include <yarp/os/BufferedPort.h>
#include <yarp/os/Semaphore.h>
#include <yarp/os/RateThread.h>
#include <yarp/os/Bottle.h>

#include <iCub/ctrl/pids.h>

#include <iCub/gazeNlp.h>
#include <iCub/utils.h>

#include <string>

using namespace std;
using namespace yarp::os;
using namespace yarp::sig;
using namespace yarp::math;
using namespace iCub::ctrl;
using namespace iCub::iKin;


// The thread launched by the application which is
// in charge of localizing target 3D position from
// image coordinates.
class Localizer : public RateThread
{
protected:
    Semaphore             mutex;
    exchangeData         *commData;
    xdPort               *port_xd;
    BufferedPort<Bottle>  port_mono;
    BufferedPort<Bottle>  port_stereo;
    BufferedPort<Bottle>  port_anglesIn;
    BufferedPort<Vector>  port_anglesOut;

    string localName;
    string camerasFile;
    unsigned int period;
    bool headV2;
    
    iCubEye  *eyeL;
    iCubEye  *eyeR;
    
    Matrix eyeCAbsFrame;
    Matrix invEyeCAbsFrame;

    Matrix *PrjL, *invPrjL;
    Matrix *PrjR, *invPrjR;
    double  cxl, cyl;
    double  cxr, cyr;

    parallelPID *pid;
    string dominantEye;

    void handleMonocularInput();
    void handleStereoInput();
    void handleAnglesInput();
    void handleAnglesOutput();

public:
    Localizer(exchangeData *_commData, const string &_localName,
              const string &_camerasFile, const bool _headV2,
              const unsigned int _period);

    void   set_xdport(xdPort *_port_xd) { port_xd=_port_xd; }
    void   getPidOptions(Bottle &options);
    void   setPidOptions(const Bottle &options);
    bool   projectPoint(const string &type, const Vector &x, Vector &px);
    bool   projectPoint(const string &type, const double u, const double v,
                        const double z, Vector &x);
    bool   projectPoint(const string &type, const double u, const double v,
                        const Vector &plane, Vector &x);
    bool   triangulatePoint(const Vector &pxl, const Vector &pxr, Vector &x);
    Vector getAbsAngles(const Vector &x);
    Vector get3DPoint(const string &type, const Vector &ang);

    virtual bool threadInit();
    virtual void afterStart(bool s);
    virtual void run();
    virtual void threadRelease();
};


#endif


