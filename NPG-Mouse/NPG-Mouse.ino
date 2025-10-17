// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

// Copyright (c) 2025 Krishnanshu Mittal - krishnanshu@upsidedownlabs.tech
// Copyright (c) 2025 Deepak Khatri - deepak@upsidedownlabs.tech
// Copyright (c) 2025 Upside Down Labs - contact@upsidedownlabs.tech

// At Upside Down Labs, we create open-source DIY neuroscience hardware and software.
// Our mission is to make neuroscience affordable and accessible for everyone.
// By supporting us with your purchase, you help spread innovation and open science.
// Thank you for being part of this journey with us!

// Core includes
#include <Arduino.h>
#include <Wire.h>
#include <BleCombo.h>

// ── MPU6050 Head Movement Includes ──
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>

// ══════════════════════════════════════════════════════════════════════════════
// ─── CONTROL MAPPING CONFIGURATION ───
// ══════════════════════════════════════════════════════════════════════════════
// Head movement -> Mouse cursor movement
// Double blink -> Left mouse click
// Triple blink -> Right mouse click

// ══════════════════════════════════════════════════════════════════════════════
// ─── EASY-TO-ADJUST MOUSE CONTROL SETTINGS ───
// ══════════════════════════════════════════════════════════════════════════════

//  BASIC SETTINGS (ADJUST THESE TO FINE-TUNE)
#define MOUSE_UPDATE_RATE   12               // Update frequency: LOWER = faster updates (8-20)
#define DEADZONE            0.01             // Rest zone: HIGHER = easier to stop (0.3-2.0)
#define MIN_SENSITIVITY     0.15             // Slowest speed: LOWER = more precise (0.1-0.5)
#define MAX_SENSITIVITY     8.0              // Fastest speed: LOWER = more controlled (4.0-15.0)

//  PRECISION SETTINGS (FOR MINUTE MOVEMENTS)
#define PRECISION_ZONE      4.0              // Precision angle range: HIGHER = more precision zone (1.0-4.0)
#define PRECISION_MULTIPLIER 0.1             // Precision sensitivity: LOWER = more precise (0.2-0.6)

//  SMOOTHING SETTINGS (FOR RESPONSIVENESS)
#define MOVEMENT_SMOOTHING  0.70             // Movement filter: LOWER = more responsive (0.5-0.85)
#define VELOCITY_DECAY      0.80             // Stop speed: LOWER = stops faster (0.7-0.9)
#define STOP_THRESHOLD      0.2              // Complete stop point: LOWER = stops sooner (0.1-0.5)

//  ACCELERATION SETTINGS
#define ACCEL_CURVE         2.5              // Acceleration curve: HIGHER = faster acceleration (1.5-4.0)
#define ACCEL_MULTIPLIER    2.8              // Acceleration strength: HIGHER = more acceleration (2.0-4.0)

//  RANGE SETTINGS
#define MAX_TILT_ANGLE      20.0             // Maximum head tilt: LOWER = shorter range (15.0-30.0)

// ══════════════════════════════════════════════════════════════════════════════

// ── VIBRATION MOTOR PIN ──
#define VIBRATION_PIN       7                // Vibration motor for calibration feedback

// ─── MPU6050 Head Movement Variables ───
Adafruit_MPU6050 mpu;

// Mouse control variables with velocity-based stopping
float neutralPitch = 0, neutralRoll = 0;
float smoothedPitch = 0, smoothedRoll = 0;
float mouseVelocityX = 0, mouseVelocityY = 0;  // Velocity for smooth stopping
float lastDeltaPitch = 0, lastDeltaRoll = 0;   // Track previous frame deltas
bool isMPUCalibrated = false;
unsigned long lastMouseUpdate = 0;

// ── AXIS CALIBRATION VARIABLES ──
int pitchDirection = 1;  // 1 = normal, -1 = inverted
int rollDirection = 1;   // 1 = normal, -1 = inverted
bool axisCalibrated = false;

// ── NON-BLOCKING CALIBRATION STATE MACHINE ──
enum CalibrationState {
  CAL_IDLE,
  CAL_INIT_WAIT,
  CAL_UP_VIBRATE,
  CAL_UP_WAIT,
  CAL_CENTER_WAIT1,
  CAL_LEFT_VIBRATE,
  CAL_LEFT_WAIT,
  CAL_CENTER_WAIT2,
  CAL_NEUTRAL_SAMPLE,
  CAL_COMPLETE
};

CalibrationState calState = CAL_IDLE;
unsigned long calStateStartTime = 0;
float calStartPitch = 0, calEndPitch = 0;
float calStartRoll = 0, calEndRoll = 0;
int neutralSampleCount = 0;
float neutralPitchSum = 0, neutralRollSum = 0;

// ─── EEG Signal processing config ───
#define SAMPLE_RATE   512   
#define INPUT_PIN1    A0    // EEG input only

// EEG Envelope Configuration for blink detection
#define ENVELOPE_WINDOW_MS 100
#define ENVELOPE_WINDOW_SIZE ((ENVELOPE_WINDOW_MS * SAMPLE_RATE) / 1000)

// Double/Triple Blink Configuration
const unsigned long BLINK_DEBOUNCE_MS = 250;
const unsigned long DOUBLE_BLINK_MS   = 600;
unsigned long lastBlinkTime   = 0;
unsigned long firstBlinkTime  = 0;
unsigned long secondBlinkTime = 0;
unsigned long triple_blink_ms = 800;
int blinkCount = 0;
bool blinkActive = false;

float envelopeBuffer[ENVELOPE_WINDOW_SIZE] = {0};
int envelopeIndex = 0;
float envelopeSum = 0;
float currentEEGEnvelope = 0;
float BlinkThreshold = 50.0;

// ─── FILTERS ───

// Band-Stop Butterworth IIR digital filter
// Sampling rate: 512.0 Hz, frequency: [48.0, 52.0] Hz
// Filter is order 2, implemented as second-order sections (biquads)
class NotchFilter {
private:
  struct BiquadState { float z1 = 0, z2 = 0; };
  BiquadState state0;
  BiquadState state1;

public:
  float process(float input) {
    float output = input;

    // Biquad section 0
    float x0 = output - (-1.58696045f * state0.z1) - (0.96505858f * state0.z2);
    output = 0.96588529f * x0 + -1.57986211f * state0.z1 + 0.96588529f * state0.z2;
    state0.z2 = state0.z1;
    state0.z1 = x0;

    // Biquad section 1
    float x1 = output - (-1.62761184f * state1.z1) - (0.96671306f * state1.z2);
    output = 1.00000000f * x1 + -1.63566226f * state1.z1 + 1.00000000f * state1.z2;
    state1.z2 = state1.z1;
    state1.z1 = x1;

    return output;
  }

  void reset() {
    state0.z1 = state0.z2 = 0;
    state1.z1 = state1.z2 = 0;
  }
} eegNotchFilter;  // Only one filter needed for EEG

// High-Pass Butterworth IIR digital filter
// Sampling rate: 512.0 Hz, frequency: 5.0 Hz
class EOGFilter {
private:
  struct BiquadState { float z1 = 0, z2 = 0; };
  BiquadState state0;

public:
  float process(float input) {
    float output = input;

    // Biquad section 0
    float x0 = output - (-1.91327599f * state0.z1) - (0.91688335f * state0.z2);
    output = 0.95753983f * x0 + -1.91507967f * state0.z1 + 0.95753983f * state0.z2;
    state0.z2 = state0.z1;
    state0.z1 = x0;

    return output;
  }

  void reset() {
    state0.z1 = state0.z2 = 0;
  }
} eogFilter;

// Low-Pass Butterworth IIR digital filter
// Sampling rate: 512.0 Hz, frequency: 45.0 Hz
class EEGFilter {
private:
    struct BiquadState { float z1 = 0, z2 = 0; };
    BiquadState state0;

public:
    float process(float input) {
        float output = input;

        // Biquad section 0
        float x0 = output - (-1.24200128f * state0.z1) - (0.45885207f * state0.z2);
        output = 0.05421270f * x0 + 0.10842539f * state0.z1 + 0.05421270f * state0.z2;
        state0.z2 = state0.z1;
        state0.z1 = x0;

        return output;
    }

    void reset() {
        state0.z1 = state0.z2 = 0;
    }
} eegFilter;

float updateEnvelope(float sample) {
  float absSample = fabsf(sample);
  envelopeSum -= envelopeBuffer[envelopeIndex];
  envelopeSum += absSample;
  envelopeBuffer[envelopeIndex] = absSample;
  envelopeIndex = (envelopeIndex + 1) % ENVELOPE_WINDOW_SIZE;
  return envelopeSum / ENVELOPE_WINDOW_SIZE;
}

// ─── VIBRATION FEEDBACK FUNCTIONS ───
void startVibration() {
  digitalWrite(VIBRATION_PIN, HIGH);
}

void stopVibration() {
  digitalWrite(VIBRATION_PIN, LOW);
}

// ─── NON-BLOCKING CALIBRATION STATE MACHINE ───
void updateCalibrationStateMachine(unsigned long nowMs) {
  if (calState == CAL_IDLE || calState == CAL_COMPLETE) return;
  
  unsigned long elapsed = nowMs - calStateStartTime;
  sensors_event_t a, g, temp;
  
  switch (calState) {
    case CAL_INIT_WAIT:
      if (elapsed >= 3000) {  // 3 second initial wait
        calState = CAL_UP_VIBRATE;
        calStateStartTime = nowMs;
        startVibration();
        // Get baseline pitch
        mpu.getEvent(&a, &g, &temp);
        calStartPitch = atan2(-a.acceleration.x, sqrt(a.acceleration.y * a.acceleration.y + a.acceleration.z * a.acceleration.z)) * 180.0 / PI;
      }
      break;
      
    case CAL_UP_VIBRATE:
      if (elapsed >= 3000) {  // 3 second vibration for UP movement
        stopVibration();
        calState = CAL_UP_WAIT;
        calStateStartTime = nowMs;
        // Get end pitch position
        mpu.getEvent(&a, &g, &temp);
        calEndPitch = atan2(-a.acceleration.x, sqrt(a.acceleration.y * a.acceleration.y + a.acceleration.z * a.acceleration.z)) * 180.0 / PI;
        // Determine pitch direction
        pitchDirection = ((calEndPitch - calStartPitch) > 0) ? -1 : 1;
      }
      break;
      
    case CAL_UP_WAIT:
      if (elapsed >= 3000) {  // 3 second wait to return to center
        calState = CAL_LEFT_VIBRATE;
        calStateStartTime = nowMs;
        startVibration();
        // Get baseline roll
        mpu.getEvent(&a, &g, &temp);
        calStartRoll = atan2(a.acceleration.y, a.acceleration.z) * 180.0 / PI;
      }
      break;
      
    case CAL_LEFT_VIBRATE:
      if (elapsed >= 3000) {  // 3 second vibration for LEFT movement
        stopVibration();
        calState = CAL_LEFT_WAIT;
        calStateStartTime = nowMs;
        // Get end roll position
        mpu.getEvent(&a, &g, &temp);
        calEndRoll = atan2(a.acceleration.y, a.acceleration.z) * 180.0 / PI;
        // Determine roll direction
        rollDirection = ((calEndRoll - calStartRoll) > 0) ? -1 : 1;
        axisCalibrated = true;
      }
      break;
      
    case CAL_LEFT_WAIT:
      if (elapsed >= 2000) {  // 2 second wait to return to center
        calState = CAL_NEUTRAL_SAMPLE;
        calStateStartTime = nowMs;
        neutralSampleCount = 0;
        neutralPitchSum = 0;
        neutralRollSum = 0;
      }
      break;
      
    case CAL_NEUTRAL_SAMPLE:
      if (neutralSampleCount < 100) {
        mpu.getEvent(&a, &g, &temp);
        float pitch = atan2(-a.acceleration.x, sqrt(a.acceleration.y * a.acceleration.y + a.acceleration.z * a.acceleration.z)) * 180.0 / PI;
        float roll = atan2(a.acceleration.y, a.acceleration.z) * 180.0 / PI;
        neutralPitchSum += pitch;
        neutralRollSum += roll;
        neutralSampleCount++;
      } else {
        // Calibration complete
        neutralPitch = neutralPitchSum / 100;
        neutralRoll = neutralRollSum / 100;
        smoothedPitch = neutralPitch;
        smoothedRoll = neutralRoll;
        isMPUCalibrated = true;
        calState = CAL_COMPLETE;
      }
      break;
      
    default:
      break;
  }
}

// ─── PRECISION MOUSE CONTROL WITH EASY ADJUSTMENT ───
void updatePrecisionMouse(unsigned long nowMs) {
  if (!isMPUCalibrated || !axisCalibrated || !Keyboard.isConnected()) return;
  
  // High frequency updates for responsiveness
  if (nowMs - lastMouseUpdate < MOUSE_UPDATE_RATE) return;
  lastMouseUpdate = nowMs;
  
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);
  
  // Calculate current head orientation
  float currentPitch = atan2(-a.acceleration.x,
                            sqrt(a.acceleration.y * a.acceleration.y + a.acceleration.z * a.acceleration.z)) * 180.0 / PI;
  float currentRoll = atan2(a.acceleration.y, a.acceleration.z) * 180.0 / PI;
  
  // Apply smoothing filter for stability
  smoothedPitch = MOVEMENT_SMOOTHING * smoothedPitch + (1.0f - MOVEMENT_SMOOTHING) * currentPitch;
  smoothedRoll = MOVEMENT_SMOOTHING * smoothedRoll + (1.0f - MOVEMENT_SMOOTHING) * currentRoll;
  
  // Calculate movement deltas relative to neutral position
  float deltaPitch = smoothedPitch - neutralPitch;
  float deltaRoll = smoothedRoll - neutralRoll;
  
  // Apply deadzone for easier control
  if (abs(deltaPitch) < DEADZONE) deltaPitch = 0;
  if (abs(deltaRoll) < DEADZONE) deltaRoll = 0;
  
  // Calculate target velocity with precision zones
  float targetVelocityX = 0, targetVelocityY = 0;
  
  if (deltaRoll != 0) {
    float absRoll = abs(deltaRoll);
    float rollDirection_sign = (deltaRoll > 0) ? 1.0f : -1.0f;
    
    // Normalize angle for acceleration calculation
    float normalizedRoll = constrain(absRoll / MAX_TILT_ANGLE, 0.0f, 1.0f);
    
    float rollSensitivity;
    
    // Check if in precision zone (small movements)
    if (absRoll <= PRECISION_ZONE) {
      // Ultra-precise control for minute movements
      rollSensitivity = MIN_SENSITIVITY * PRECISION_MULTIPLIER * (absRoll / PRECISION_ZONE);
    } else {
      // Normal acceleration curve for larger movements
      float acceleration = pow(normalizedRoll, ACCEL_CURVE);
      rollSensitivity = MIN_SENSITIVITY + (MAX_SENSITIVITY - MIN_SENSITIVITY) * acceleration * ACCEL_MULTIPLIER;
    }
    
    targetVelocityX = rollDirection_sign * rollSensitivity * rollDirection;
  }
  
  if (deltaPitch != 0) {
    float absPitch = abs(deltaPitch);
    float pitchDirection_sign = (deltaPitch > 0) ? 1.0f : -1.0f;
    
    // Normalize angle for acceleration calculation
    float normalizedPitch = constrain(absPitch / MAX_TILT_ANGLE, 0.0f, 1.0f);
    
    float pitchSensitivity;
    
    // Check if in precision zone (small movements)
    if (absPitch <= PRECISION_ZONE) {
      // Ultra-precise control for minute movements
      pitchSensitivity = MIN_SENSITIVITY * PRECISION_MULTIPLIER * (absPitch / PRECISION_ZONE);
    } else {
      // Normal acceleration curve for larger movements
      float acceleration = pow(normalizedPitch, ACCEL_CURVE);
      pitchSensitivity = MIN_SENSITIVITY + (MAX_SENSITIVITY - MIN_SENSITIVITY) * acceleration * ACCEL_MULTIPLIER;
    }
    
    targetVelocityY = pitchDirection_sign * pitchSensitivity * pitchDirection;
  }
  
  // Velocity-based movement with smooth decay for stopping
  if (deltaRoll == 0 || targetVelocityX == 0) {
    // No input - decay velocity for smooth stopping
    mouseVelocityX *= VELOCITY_DECAY;
    if (abs(mouseVelocityX) < STOP_THRESHOLD) mouseVelocityX = 0;
  } else {
    // Active input - blend toward target velocity smoothly
    mouseVelocityX = mouseVelocityX * 0.8f + targetVelocityX * 0.2f;
  }
  
  if (deltaPitch == 0 || targetVelocityY == 0) {
    // No input - decay velocity for smooth stopping
    mouseVelocityY *= VELOCITY_DECAY;
    if (abs(mouseVelocityY) < STOP_THRESHOLD) mouseVelocityY = 0;
  } else {
    // Active input - blend toward target velocity smoothly
    mouseVelocityY = mouseVelocityY * 0.8f + targetVelocityY * 0.2f;
  }
  
  // Store current deltas for next frame
  lastDeltaPitch = deltaPitch;
  lastDeltaRoll = deltaRoll;
  
  // Convert to integer movement
  int finalMouseX = round(mouseVelocityX);
  int finalMouseY = round(mouseVelocityY);
  
  // Send precise mouse movement
  if (finalMouseX != 0 || finalMouseY != 0) {
    Mouse.move(finalMouseX, finalMouseY);
  }
}

// ─── setup() ───
void setup() {
  Wire.begin(22, 23);

  // Initialize ADC pins - only EEG channel
  pinMode(INPUT_PIN1, INPUT);
  pinMode(VIBRATION_PIN, OUTPUT);

  // Initialize vibration motor (OFF)
  digitalWrite(VIBRATION_PIN, LOW);

  // Initialize BLE Combo (Keyboard + Mouse) - no custom name parameter
  Keyboard.begin();
  Mouse.begin();

  // Initialize MPU6050
  if (!mpu.begin()) {
    isMPUCalibrated = false;
    axisCalibrated = false;
    calState = CAL_COMPLETE;  // Skip calibration if MPU not found
  } else {
    mpu.setAccelerometerRange(MPU6050_RANGE_2_G);
    mpu.setGyroRange(MPU6050_RANGE_250_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
    
    // START NON-BLOCKING CALIBRATION
    calState = CAL_INIT_WAIT;
    calStateStartTime = millis();
  }
}

// ─── loop() ───
void loop() {
  static unsigned long lastMicros = micros();

  unsigned long now = micros(), dt = now - lastMicros;
  lastMicros = now;
  static long timer = 0;
  timer -= dt;
  
  if (timer <= 0) {
    timer += 1000000L / SAMPLE_RATE;
    unsigned long nowMs = millis();

    // NON-BLOCKING CALIBRATION UPDATE
    updateCalibrationStateMachine(nowMs);

    // 1) EEG ADC read - only one channel
    int raw1 = analogRead(INPUT_PIN1);

    // 2) EEG filtering and envelope for blinks
    float filteeg = eegFilter.process(eegNotchFilter.process(raw1));
    float filteog = eogFilter.process(filteeg);
    currentEEGEnvelope = updateEnvelope(filteog);

    // 3) Blink detection - Double blink = Left click, Triple blink = Right click
    bool envelopeHigh = currentEEGEnvelope > BlinkThreshold;
    if (!blinkActive && envelopeHigh && (nowMs - lastBlinkTime) >= BLINK_DEBOUNCE_MS) {
      lastBlinkTime = nowMs;
      if (blinkCount == 0) {
        firstBlinkTime = nowMs; blinkCount = 1;
      } else if (blinkCount == 1 && (nowMs - firstBlinkTime) <= DOUBLE_BLINK_MS) {
        secondBlinkTime = nowMs; blinkCount = 2;
      } else if (blinkCount == 2 && (nowMs - secondBlinkTime) <= triple_blink_ms) {
        // Triple blink detected -> Right mouse click (press once)
        Mouse.click(MOUSE_RIGHT);
        blinkCount = 0;
      } else {
        firstBlinkTime = nowMs; blinkCount = 1;
      }
      blinkActive = true;
    }

    if (!envelopeHigh) {
      blinkActive = false; 
    }

    // Double blink timeout -> Left mouse click (press once)
    if (blinkCount == 2 && (nowMs - secondBlinkTime) > triple_blink_ms) {
      Mouse.click(MOUSE_LEFT);  // Left click for double blink
      blinkCount = 0;
    }
    // Single blink timeout
    if (blinkCount == 1 && (nowMs - firstBlinkTime) > DOUBLE_BLINK_MS) {
      blinkCount = 0;
    }
  }

  // 4) PRECISION MOUSE CONTROL - runs continuously
  updatePrecisionMouse(millis());
}
