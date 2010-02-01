/*******************************************************************************
 * Copyright (C) 2009-2010 Christian Wressnegger
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *******************************************************************************/

#include "iCub/vislab/HandModule.h"

#include <algorithm>
#include <cmath>

using namespace std;

using namespace yarp::os;
using namespace yarp::sig;
using namespace yarp::math;
using namespace yarp::dev;

using namespace vislab::util;
using namespace vislab::yarp::util;
using namespace vislab::yarp;

namespace vislab {
namespace control {

HandModule::HandModule(ConstString name, double period) :
  ThreadedRFModule(name, period) {
}

HandModule::~HandModule() {
}

bool HandModule::ReloadCommand::execute(const Bottle& params, Bottle& reply) const {
  HandModule* h = (HandModule*) parent;
  NetInt32 result = Vocab::encode("ack");

  try {
    h->motionSpecification.fromConfigFile(h->motionSpecificationFilename);
    h->workerThread->addMotionSpecification(h->motionSpecification);
  } catch (exception&) {
    result = Vocab::encode("fail");
  }

  reply.addVocab(result);
  return true;
}

bool HandModule::configure(ResourceFinder &rf) {
  if (!ThreadedRFModule::configure(rf)) {
    return false;
  }

  ConstString str;

  partName = rf.check("part", Value("right_arm"),
      "The robot's arm to work with (\"right_arm\" or \"left_arm\")").asString();

  // TODO: remove this once they finally resolved the "/" confusion
  str = getName(prefix + partName);
  replaceDoubleSlash(str);
  setName(str);

  if (!isHandlerAvailable()) {
    return false; // unable to open; let RFModule know so that it won't run
  }

  ConstString handType = rf.check("handType", Value("general"),
      "The typ of the icub's hands: \"general\" | \"v1\" (string)").asString();

  if (handType == "v1") {
    rf.setDefault("sensingCalib", "object_sensing.ini");
    // str = "Name of the configuration file specifying the object sensing constants (string)";
    ConstString sensingCalibrationFile = rf.findFile("sensingCalib"/*, str*/);

    Property p;
    if (!p.fromConfigFile(sensingCalibrationFile)) {
      cerr << "Unable to read sensing calibration. "
          << "Specify a valid file name or consider using the \"general\" hand type" << endl;
      return false;
    }

    string part = partName.c_str();
    transform(part.begin(), part.end(), part.begin(), ::toupper);
    sensingCalibration = p.findGroup(part.c_str());

    this->handType = v1;
  } else {
    this->handType = GENERAL;
  }

  rf.setDefault("motionSpec", "motion_specification.ini");
  motionSpecificationFilename = rf.findFile("motionSpec"/*,
   "Name of the configuration file specifying the motions (string)"*/);

  motionSpecification.fromConfigFile(motionSpecificationFilename);

  str = prefix + getName(rf.check("control", Value("/control"),
      "Control port for communicating with the control board").asString());
  dataPorts.add(id.Control, str, NULL); // opened by PolyDriver!

  ConstString controlboardName(prefix + robotName + prefix + partName);

  Property p;
  p.put("device", "remote_controlboard");
  p.put("local", dataPorts.getName(id.Control)); // client
  p.put("remote", controlboardName); // server
  Bottle config(p.toString());

  if (!controlBoard.open(config)) {
    cerr << "Could not open remote control board (port name: " << controlboardName << ")" << endl;
    return false;
  }

  IEncoders* encoders;
  if (!controlBoard.view(encoders)) {
    cerr << "Motor control board does not provide encoders." << endl;
    return false;
  }

  IPidControl* pidControl;
  if (!controlBoard.view(pidControl)) {
    cerr << "Motor control board does not provide PID control." << endl;
    return false;
  }

  IPositionControl* posControl;
  if (!controlBoard.view(posControl)) {
    cerr << "Motor control board does not provide position control." << endl;
    return false;
  }

  IAmplifierControl* ampControl;
  if (!controlBoard.view(ampControl)) {
    cerr << "Motor control board does not provide amplifier control." << endl;
    return false;
  }

  int numAxes;
  encoders->getAxes(&numAxes);
  if (numAxes != HandMetrics::numAxes) {
    cerr << "The number of axes mismatches the expected number. "
        << "It might be that the remote control board to connect to " << controlboardName << endl;
    return false;
  }

  return true;
}

bool HandModule::close() {
  bool b = controlBoard.close();
  return ThreadedRFModule::close() && b;
}

bool HandModule::startThread() {
  bool b = ThreadedRFModule::startThread();
  workerThread = dynamic_cast<HandWorkerThread *> (ThreadedRFModule::workerThread);
  return b;
}

//#define DEBUG

HandModule::HandWorkerThread::HandWorkerThread(const OptionManager& moduleOptions,
    const Contactables& ports, PolyDriver& controlBoard, HandType t) :
  RFWorkerThread(moduleOptions, ports), controlBoard(controlBoard) {

  handType = t;
  hand = NULL;
  handv1 = NULL;

  // configure control board
  bool isValid = true;
  isValid &= controlBoard.view(encoders);
  isValid &= controlBoard.view(pidControl);
  isValid &= controlBoard.view(posControl);
  isValid &= controlBoard.view(ampControl);

  if (!isValid) {
    throw invalid_argument("Insufficient control board");
  }

  createHand();
}

void HandModule::HandWorkerThread::createHand() {
  // in case createHand is called more than once
  if (hand != NULL) {
    delete hand;
  }
  switch (handType) {
  case v1:
    try {
      handv1 = new Handv1(controlBoard, sensingConstants);
      HandModule::HandWorkerThread::hand = (Hand*) handv1;
    } catch (exception&) {
      hand = new Hand(controlBoard);
    }
    break;
  case GENERAL:
  default:
    hand = new Hand(controlBoard);
    break;
  }
}
void HandModule::HandWorkerThread::setMotionSpecification(Searchable& s) {
  motions.clear();
  addMotionSpecification(s);
}

void HandModule::HandWorkerThread::addMotionSpecification(Searchable& s) {
  Bottle b(s.toString());
  readMotionSequences(b, motions);

#ifdef DEBUG
  map<const string, MotionSequence>::const_iterator itr1;
  for (itr1 = motions.begin(); itr1 != motions.end(); ++itr1) {
    cout << "Motion type: " << itr1->first << endl;
    cout << itr1->second.toString() << endl;
  }
#endif
}

const map<const string, MotionSequence>& HandModule::HandWorkerThread::getMotionSpecifications() {
  return motions;
}

void HandModule::HandWorkerThread::setSensingConstants(::yarp::os::Searchable& s) {
  if (handType != v1) {
    cout << "Warning: You are trying to set sensing constants which is not"
        << " necessary/ meant for the type of hand you specified." << endl;
  }

  Bottle b(s.toString());
  sensingConstants.clear();
  readMatrices(b, sensingConstants);

#ifdef DEBUG
  cout << "Sensing Constants" << endl;
  map<const string, Matrix>::const_iterator itr2;
  for (itr2 = sensingConstants.begin(); itr2 != sensingConstants.end(); ++itr2) {
    cout << (*itr2).first << "..." << endl;
    printMatrix((*itr2).second);
  }
#endif

  string osValueNames[] = { "thresholds", "offsets", "springs", "derivate_gain" };

  for (int i = 0; i < 4; i++) {
    Matrix& m = sensingConstants[osValueNames[i]];

    if (m.cols() != HandMetrics::numAxes) {
      cout << "Warning: The expected size of `" << osValueNames[i] << "` is "
          << HandMetrics::numAxes << ", but it was " << m.cols()
          << ". The sizes will be automatically adopted!" << endl;
      resize(m, max(1, m.rows()), HandMetrics::numAxes);
    }
  }
  createHand();
}

HandModule::HandWorkerThread::~HandWorkerThread() {
  // Since derived classes are supposed to create the Hand object,
  // we cannot rely on its existence.
  if (hand != NULL) {
    delete hand;
  }
}

void HandModule::HandWorkerThread::setEnable(const std::vector<int>& joints, const bool b) {
  hand->setEnable(joints, b);
}

void HandModule::HandWorkerThread::getDisabledJoints(std::vector<int>& joints) {
  hand->getDisabledJoints(joints);
}

}
}
