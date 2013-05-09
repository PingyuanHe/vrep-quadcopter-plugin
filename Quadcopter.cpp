// -*- Mode: C++; indent-tabs-mode: nil; c-basic-offset: 2 -*-
//
// Quadcopter.cpp
//
// Copyright (C) 2013, Galois, Inc.
// All Rights Reserved.
//

#include <stdint.h>
#include <stdio.h>

#include <deque>
#include <map>
#include <stdexcept>
#include <vector>

#include "v_repLib.h"
#include "Quadcopter.h"

// Header number for our custom data.
#define DATA_ID 1000

// Field IDs for our custom data.
#define FIELD_QUADCOPTER     0
#define FIELD_MOTOR_0        1
#define FIELD_MOTOR_1        2
#define FIELD_MOTOR_2        3
#define FIELD_MOTOR_3        4
#define FIELD_CAMERA_DOWN    5
#define FIELD_CAMERA_FRONT   6
#define FIELD_BODY           7
#define FIELD_TARGET         8

// Our custom data is stored in the same format as the V-REP plug-in
// tutorial:
//
// 1000,{field_id,field_len,int x field_len}*

//////////////////////////////////////////////////////////////////////
// Utilities
//
// These could potentially move into a separate module.

// Shorthand for a vector of bytes.
typedef std::vector<uint8_t> byte_vector;

// Mapping of field IDs to their data.
typedef std::map<uint32_t, byte_vector> CustomData;

// Read a little endian integer from an iterator.  If the distance
// between "start" and "end" is too small, this throws an exception.
// The "start" iterator is modified to point at the next byte
// following the integer that was read.
static uint32_t parseUint32(byte_vector::const_iterator& p,
                            byte_vector::const_iterator end)
{
  if (end - p < 4)
    throw std::runtime_error("custom data format error");

  uint32_t result = (((uint32_t) p[0] << 0)  |
                     ((uint32_t) p[1] << 8)  |
                     ((uint32_t) p[2] << 16) |
                     ((uint32_t) p[3] << 24));

  p += 4;
  return result;
}

// Parse a custom data buffer into a list of fields.
static CustomData parseCustomData(const std::vector<uint8_t>& buf)
{
  CustomData result;
  auto p = buf.begin();
  auto end = buf.end();

  while (p != end) {
    uint32_t id  = parseUint32(p, end);
    uint32_t len = parseUint32(p, end);
    result[id]   = byte_vector(p, p + len);
    p += len;
  }

  return result;
}

// Return true if an object contains a custom data field.
static bool hasCustomDataField(int obj, uint32_t field)
{
  int size = simGetObjectCustomDataLength(obj, DATA_ID);
  if (size <= 0)
    return false;

  std::vector<uint8_t> buf(size);
  simGetObjectCustomData(obj, DATA_ID, (simChar *)&buf[0]);
  CustomData data(parseCustomData(buf));

  return data.find(field) != data.end();
}

// Search an object tree for an object that has the specified custom
// data field.  Returns the first matching object ID or -1 if no child
// object with that field is found.  The search is performed in
// breadth-first order.
static int searchCustomDataField(int root, uint32_t field)
{
  std::deque<int> q;
  q.push_back(root);

  while (!q.empty()) {
    int obj = q.front();
    q.pop_front();

    if (hasCustomDataField(obj, field))
      return obj;

    int i = 0;
    for (;;) {
      int child = simGetObjectChild(obj, i++);
      if (child == -1)
        break;

      q.push_back(child);
    }
  }

  return -1;
}

bool Quadcopter::query(int obj)
{
  return hasCustomDataField(obj, FIELD_QUADCOPTER);
}

// Print an object with a label for debugging.
static void printObjWithLabel(const std::string& name, int obj)
{
  fprintf(stderr, "%-12s id %d", name.c_str(), obj);

  if (obj != -1) {
    simChar *objName = simGetObjectName(obj);
    fprintf(stderr, " name '%s'", objName);
    simReleaseBuffer(objName);
  }

  fprintf(stderr, "\n");
}

// Write a camera's image data as a PPM image to a file.
static bool writeCameraPPM(const std::string& filename, int obj)
{
  simInt size[2];
  simFloat *image;

  fprintf(stderr, "saving image to file '%s'...\n", filename.c_str());

  if (simGetVisionSensorResolution(obj, size) == -1) {
    fprintf(stderr, "getting camera resolution failed\n");
    return false;
  }

  if ((image = simGetVisionSensorImage(obj)) == NULL) {
    fprintf(stderr, "getting camera image failed\n");
    return false;
  }

  int pixel_size = size[0] * size[1] * 3;
  std::vector<uint8_t> data(pixel_size);

  for (int i = 0; i < pixel_size; ++i)
    data[i] = (uint8_t)(image[i] * 255.0f);

  FILE *f;
  if ((f = fopen(filename.c_str(), "wb")) == NULL) {
    fprintf(stderr, "saving image failed\n");
    return false;
  }

  fprintf(f, "P6 %d %d 255\n", size[0], size[1]);
  fwrite(&data[0], 1, pixel_size, f);
  fclose(f);

  return true;
}

Quadcopter::Quadcopter(int obj)
  : m_obj(obj)
{
  simGetObjectUniqueIdentifier(obj, &m_uniqueID);

  m_body        = searchCustomDataField(obj, FIELD_BODY);
  m_target      = searchCustomDataField(obj, FIELD_TARGET);
  m_cameraDown  = searchCustomDataField(obj, FIELD_CAMERA_DOWN);
  m_cameraFront = searchCustomDataField(obj, FIELD_CAMERA_FRONT);

  m_motors[0]   = searchCustomDataField(obj, FIELD_MOTOR_0);
  m_motors[1]   = searchCustomDataField(obj, FIELD_MOTOR_1);
  m_motors[2]   = searchCustomDataField(obj, FIELD_MOTOR_2);
  m_motors[3]   = searchCustomDataField(obj, FIELD_MOTOR_3);

  fprintf(stderr, "--- Found Quadcopter %d:\n", m_uniqueID);
  printObjWithLabel("Quadcopter:", m_obj);
  printObjWithLabel("Body:",       m_body);
  printObjWithLabel("Target:",     m_target);
  printObjWithLabel("Motor #1:",   m_motors[0]);
  printObjWithLabel("Motor #2:",   m_motors[1]);
  printObjWithLabel("Motor #3:",   m_motors[2]);
  printObjWithLabel("Motor #4:",   m_motors[3]);
  printObjWithLabel("Floor Cam:",  m_cameraDown);
  printObjWithLabel("Front Cam:",  m_cameraFront);
}

void Quadcopter::simulationStarted()
{
  m_last_save_time = 0;
  m_cumul          = 0.0f;
  m_lastE          = 0.0f;
  m_pAlphaE        = 0.0f;
  m_pBetaE         = 0.0f;
  m_psp0           = 0.0f;
  m_psp1           = 0.0f;
  m_prevEuler      = 0.0f;
}

void Quadcopter::simulationStopped()
{
}

void Quadcopter::setMotorParticleVelocity(int n, float v)
{
  if (n < 0 || n > 3)
    return;

  int script = simGetScriptAssociatedWithObject(m_motors[n]);
  if (script == -1)
    return;

  std::string s(std::to_string(v));
  simSetScriptSimulationParameter(
    script, "particleVelocity", &s[0], s.size());
}

float Quadcopter::getMotorParticleVelocity(int n) const
{
  if (n < 0 || n > 3)
    return 0.0f;

  float result = 0.0f;
  simInt size;
  int script = simGetScriptAssociatedWithObject(m_motors[n]);

  if (script == -1) {
    fprintf(stderr, "getting motor script failed\n");
    return 0.0f;
  }

  simChar *r = simGetScriptSimulationParameter(
    script, "particleVelocity", &size);

  if (r != NULL) {
    std::string s(r);
    simReleaseBuffer(r);
    result = std::stof(s);
  } else {
    fprintf(stderr, "getting motor velocity failed\n");
  }

  return result;
}

// Error checking macro for "pidControl".  This wraps calls to the
// V-REP API functions and prints an error message and returns from
// the current function if an error occurs.
#define CHECK(expr)                               \
  do {                                            \
    if ((expr) == -1) {                           \
      fprintf(stderr, "%s:%d: %s failed\n",       \
              __FILE__, __LINE__, # expr);        \
      return;                                     \
    }                                             \
  } while (0)

// Ported PID control from the Lua script.  Follows the quadcopter
// target object.
void Quadcopter::pidControl()
{
  static const float pParam =  1.0f;
  static const float iParam =  0.0f;
  static const float dParam =  0.0f;
  static const float vParam = -2.0f;
  int d = m_body;               // to match lua script

  // Vertical control:
  float targetPos[3], pos[3], vel[3];
  float error, pv, thrust;

  CHECK(simGetObjectPosition(m_target, -1, targetPos));
  CHECK(simGetObjectPosition(d, -1, pos));
  CHECK(simGetObjectVelocity(m_obj, vel, NULL));

  error    = targetPos[2] - pos[2];
  m_cumul += error;
  pv       = pParam * error;
  thrust   = 5.335f + pv + iParam*m_cumul + dParam*(error - m_lastE) + vel[2]*vParam;
  m_lastE  = error;

  // Horizontal control:
  float sp[3];
  float m[12];
  float vx[3] = { 1.0f, 0.0f, 0.0f };
  float vy[3] = { 0.0f, 1.0f, 0.0f };

  CHECK(simGetObjectPosition(m_target, d, sp));
  CHECK(simGetObjectMatrix(d, -1, m));
  CHECK(simTransformVector(m, vx));
  CHECK(simTransformVector(m, vy));

  float alphaE    =  vy[2] - m[11];
  float alphaCorr =  0.25f * alphaE + 2.1f * (alphaE - m_pAlphaE);
  float betaE     =  vx[2] - m[11];
  float betaCorr  = -0.25f * betaE  - 2.1f * (betaE  - m_pBetaE);

  m_pAlphaE = alphaE;
  m_pBetaE  = betaE;

  alphaCorr = alphaCorr + sp[1] * 0.005f + 1.0f * (sp[1] - m_psp1);
  betaCorr  = betaCorr  - sp[0] * 0.005f - 1.0f * (sp[0] - m_psp0);
  m_psp0    = sp[0];
  m_psp1    = sp[1];

  // Rotational control:
  float rotCorr, euler[3];
  CHECK(simGetObjectOrientation(d, m_target, euler));
  rotCorr     = euler[2] * 0.1f + 2.0f * (euler[2] - m_prevEuler);
  m_prevEuler = euler[2];

  float motorVel[4];
  motorVel[0] = thrust * (1.0f - alphaCorr + betaCorr + rotCorr);
  motorVel[1] = thrust * (1.0f - alphaCorr - betaCorr - rotCorr);
  motorVel[2] = thrust * (1.0f + alphaCorr - betaCorr + rotCorr);
  motorVel[3] = thrust * (1.0f + alphaCorr + betaCorr - rotCorr);

  setMotorParticleVelocity(0, motorVel[0]);
  setMotorParticleVelocity(1, motorVel[1]);
  setMotorParticleVelocity(2, motorVel[2]);
  setMotorParticleVelocity(3, motorVel[3]);
}

#undef CHECK

void Quadcopter::simulationStepped()
{
  pidControl();

#if 0
  float now = simGetSimulationTime();


  if (now - m_last_save_time > 1.0f) {
    m_last_save_time = now;

    if (m_cameraDown != -1) {
      char filename[128];
      snprintf(filename, sizeof(filename), "cam%u_%u.ppm", m_obj, (int)now);
      writeCameraPPM(filename, m_cameraDown);
    }
  }
#endif
}