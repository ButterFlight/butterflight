/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "platform.h"

#include "build/debug.h"

#include "common/axis.h"
#include "common/maths.h"
#include "common/utils.h"

#include "config/feature.h"

#include "fc/config.h"
#include "fc/controlrate_profile.h"
#include "fc/fc_core.h"
#include "fc/fc_rc.h"
#include "fc/rc_controls.h"
#include "fc/rc_modes.h"
#include "fc/runtime_config.h"

#include "flight/failsafe.h"
#include "flight/imu.h"
#include "flight/pid.h"
#include "rx/rx.h"

#include "scheduler/scheduler.h"

#include "sensors/battery.h"

#ifdef USE_GYRO_IMUF9001
    volatile bool isSetpointNew;
#endif

typedef float (applyRatesFn)(const int axis, float rcCommandf, const float rcCommandfAbs);

static float rcDeflection[3], rcDeflectionAbs[3];
static volatile float setpointRate[3];
static volatile uint32_t setpointRateInt[3];
static float throttlePIDAttenuation;
static bool reverseMotors = false;
static applyRatesFn *applyRates;
static float rcCommandInterp[4] = { 0, 0, 0, 0 };
static float rcStepSize[4] = { 0, 0, 0, 0 };
static float inverseRcInt;
static uint8_t interpolationChannels;
volatile bool isRXDataNew;
volatile uint8_t skipInterpolate;
volatile int16_t rcInterpolationStepCount;
volatile uint16_t rxRefreshRate;
volatile uint16_t currentRxRefreshRate;

#define TPA_LAST_CURVE_INDEX TPA_CURVE_SIZE - 1
#define MAX_TPA_THROTTLE 1023
#define MAX_TPA_THROTTLEf 1023.0f
#define TPA_KP 0
#define TPA_KI 1
#define TPA_KD 2

float currentTpaKp;
float currentTpaKi;
float currentTpaKd;
uint16_t currentAdjustedThrottle; // rcData[THROTTLE] shifted to 0-1023 range
float percentCurve[3][TPA_CURVE_SIZE];

FAST_CODE float ApplyAttenuationCurve (float inputAttn, float curve[])
{
    float attenuationValue = (inputAttn * (TPA_LAST_CURVE_INDEX));
    float remainder = (float)(attenuationValue - (int)attenuationValue);
    uint32_t position = (int)attenuationValue;
    if (inputAttn == 1){
        return(curve[TPA_LAST_CURVE_INDEX]);
    }
    else
    {
        return(curve[position] + (((curve[position+1] - curve[position]) * remainder)));
    }
}

static void BuildTPACurve(void)
{
    // curve needs to be float'd
    for (uint8_t i = 0; i < TPA_CURVE_SIZE; i++) {
        percentCurve[TPA_KP][i] = (float)currentControlRateProfile->tpaKpCurve[i] / 100.0f;
        percentCurve[TPA_KI][i] = (float)currentControlRateProfile->tpaKiCurve[i] / 100.0f;
        percentCurve[TPA_KD][i] = (float)currentControlRateProfile->tpaKdCurve[i] / 100.0f;
    }
}

float getThrottlePIDAttenuationKp(void) {
    return currentTpaKp;
}

float getThrottlePIDAttenuationKi(void) {
    return currentTpaKi;
}

float getThrottlePIDAttenuationKd(void) {
    return currentTpaKd;
}



float getSetpointRate(int axis)
{
    return setpointRate[axis];
}

uint32_t getSetpointRateInt(int axis)
{
    return setpointRateInt[axis];
}

float getRcDeflection(int axis)
{
    return rcDeflection[axis];
}

float getRcDeflectionAbs(int axis)
{
    return rcDeflectionAbs[axis];
}

float getThrottlePIDAttenuation(void)
{
    return throttlePIDAttenuation;
}

#define THROTTLE_LOOKUP_LENGTH 12
static int16_t lookupThrottleRC[THROTTLE_LOOKUP_LENGTH];    // lookup table for expo & mid THROTTLE

static int16_t rcLookupThrottle(int32_t tmp)
{
    const int32_t tmp2 = tmp / 100;
    // [0;1000] -> expo -> [MINTHROTTLE;MAXTHROTTLE]
    return lookupThrottleRC[tmp2] + (tmp - tmp2 * 100) * (lookupThrottleRC[tmp2 + 1] - lookupThrottleRC[tmp2]) / 100;
}

#define SETPOINT_RATE_LIMIT 1998.0f
#define RC_RATE_INCREMENTAL 14.54f

float applyBetaflightRates(const int axis, float rcCommandf, const float rcCommandfAbs)
{
    if (currentControlRateProfile->rcExpo[axis]) {
        const float expof = currentControlRateProfile->rcExpo[axis] / 100.0f;
        rcCommandf = rcCommandf * power3(rcCommandfAbs) * expof + rcCommandf * (1 - expof);
    }

    float rcRate = currentControlRateProfile->rcRates[axis] / 100.0f;
    if (rcRate > 2.0f) {
        rcRate += RC_RATE_INCREMENTAL * (rcRate - 2.0f);
    }
    float angleRate = 200.0f * rcRate * rcCommandf;
    if (currentControlRateProfile->rates[axis]) {
        const float rcSuperfactor = 1.0f / (constrainf(1.0f - (rcCommandfAbs * (currentControlRateProfile->rates[axis] / 100.0f)), 0.01f, 1.00f));
        angleRate *= rcSuperfactor;
    }

    return angleRate;
}

float applyRaceFlightRates(const int axis, float rcCommandf, const float rcCommandfAbs)
{
    // -1.0 to 1.0 ranged and curved
    rcCommandf = ((1.0f + 0.01f * currentControlRateProfile->rcExpo[axis] * (rcCommandf * rcCommandf - 1.0f)) * rcCommandf);
    // convert to -2000 to 2000 range using acro+ modifier
    float angleRate = 10.0f * currentControlRateProfile->rcRates[axis] * rcCommandf;
    angleRate = angleRate * (1 + rcCommandfAbs * (float)currentControlRateProfile->rates[axis] * 0.01f);

    return angleRate;
}

static void calculateSetpointRate(int axis)
{
    // scale rcCommandf to range [-1.0, 1.0]
    float rcCommandf = rcCommand[axis] / 500.0f;
    rcDeflection[axis] = rcCommandf;
    const float rcCommandfAbs = ABS(rcCommandf);
    rcDeflectionAbs[axis] = rcCommandfAbs;

    float angleRate = applyRates(axis, rcCommandf, rcCommandfAbs);
    setpointRate[axis] = constrainf(angleRate, -SETPOINT_RATE_LIMIT, SETPOINT_RATE_LIMIT); // Rate limit protection (deg/sec)
    memcpy((uint32_t*)&setpointRateInt[axis], (uint32_t*)&setpointRate[axis], sizeof(float));
}

static void scaleRcCommandToFpvCamAngle(void)
{
    //recalculate sin/cos only when rxConfig()->fpvCamAngleDegrees changed
    static uint8_t lastFpvCamAngleDegrees = 0;
    static float cosFactor = 1.0;
    static float sinFactor = 0.0;

    if (lastFpvCamAngleDegrees != rxConfig()->fpvCamAngleDegrees) {
        lastFpvCamAngleDegrees = rxConfig()->fpvCamAngleDegrees;
        cosFactor = cos_approx(rxConfig()->fpvCamAngleDegrees * RAD);
        sinFactor = sin_approx(rxConfig()->fpvCamAngleDegrees * RAD);
    }

    float roll = setpointRate[ROLL];
    float yaw = setpointRate[YAW];
    setpointRate[ROLL] = constrainf(roll * cosFactor -  yaw * sinFactor, -SETPOINT_RATE_LIMIT, SETPOINT_RATE_LIMIT);
    setpointRate[YAW]  = constrainf(yaw  * cosFactor + roll * sinFactor, -SETPOINT_RATE_LIMIT, SETPOINT_RATE_LIMIT);
}

#define THROTTLE_BUFFER_MAX 20
#define THROTTLE_DELTA_MS 100

static void checkForThrottleErrorResetState(void)
{
    currentRxRefreshRate = constrain(getTaskDeltaTime(TASK_RX),1000,20000);

    static int index;
    static int16_t rcCommandThrottlePrevious[THROTTLE_BUFFER_MAX];

    const int rxRefreshRateMs = rxRefreshRate / 1000;
    const int indexMax = constrain(THROTTLE_DELTA_MS / rxRefreshRateMs, 1, THROTTLE_BUFFER_MAX);
    const int16_t throttleVelocityThreshold = (feature(FEATURE_3D)) ? currentPidProfile->itermThrottleThreshold / 2 : currentPidProfile->itermThrottleThreshold;

    rcCommandThrottlePrevious[index++] = rcCommand[THROTTLE];
    if (index >= indexMax) {
        index = 0;
    }

    const int16_t rcCommandSpeed = rcCommand[THROTTLE] - rcCommandThrottlePrevious[index];

    if (ABS(rcCommandSpeed) > throttleVelocityThreshold) {
        pidSetItermAccelerator(CONVERT_PARAMETER_TO_FLOAT(currentPidProfile->itermAcceleratorGain));
    } else {
        pidSetItermAccelerator(1.0f);
    }
}

void processRcCommand(void)
{
    if (skipInterpolate && !isRXDataNew) {
        skipInterpolate--;
        return;
    }
    skipInterpolate = targetPidLooptime < 120 ? 3: 0;

    int updatedChannel = 0;
    if (isRXDataNew && isAntiGravityModeActive()) {
        checkForThrottleErrorResetState();
    }

    if (rxConfig()->rcInterpolation) {
        if (isRXDataNew) {
            if (debugMode == DEBUG_RC_INTERPOLATION) {
                debug[0] = lrintf(rcCommand[0]);
                debug[1] = lrintf(getTaskDeltaTime(TASK_RX) * 0.001f);
            }

             // Set RC refresh rate for sampling and channels to filter
            switch (rxConfig()->rcInterpolation) {
                case RC_SMOOTHING_AUTO:
                    rxRefreshRate = currentRxRefreshRate + 1000; // Add slight overhead to prevent ramps
                    break;
                case RC_SMOOTHING_MANUAL:
                    rxRefreshRate = 1000 * rxConfig()->rcInterpolationInterval;
                    break;
                case RC_SMOOTHING_OFF:
                case RC_SMOOTHING_DEFAULT:
                default:
                    rxRefreshRate = rxGetRefreshRate();
            }

            rcInterpolationStepCount = rxRefreshRate / MAX(targetPidLooptime, 125u);
            inverseRcInt = 1.0f / (float)rcInterpolationStepCount;

            for (int channel = ROLL; channel < interpolationChannels; channel++) {
                rcStepSize[channel] = (rcCommand[channel] - rcCommandInterp[channel]) * inverseRcInt;
            }
        } else {
            rcInterpolationStepCount--;
        }

        // Interpolate steps of rcCommand
        if (rcInterpolationStepCount > 0) {
            for (updatedChannel = ROLL; updatedChannel < interpolationChannels; updatedChannel++) {
                rcCommandInterp[updatedChannel] += rcStepSize[updatedChannel];
                rcCommand[updatedChannel] = rcCommandInterp[updatedChannel];
            }
        }
    } else {
        rcInterpolationStepCount = 0; // reset factor in case of level modes flip flopping
    }

    if (isRXDataNew || updatedChannel) {
        const uint8_t maxUpdatedAxis = isRXDataNew ? FD_YAW : MIN(updatedChannel, FD_YAW); // throttle channel doesn't require rate calculation
#if defined(SITL)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-loop-optimizations"
#endif
        for (int axis = FD_ROLL; axis <= maxUpdatedAxis; axis++) {
#if defined(SITL)
#pragma GCC diagnostic pop
#endif
            calculateSetpointRate(axis);
        }
        #ifdef USE_GYRO_IMUF9001
        isSetpointNew = 1;
        #endif
        if (debugMode == DEBUG_RC_INTERPOLATION) {
            debug[2] = rcInterpolationStepCount;
            debug[3] = setpointRate[0];
        }

        // Scaling of AngleRate to camera angle (Mixing Roll and Yaw)
        if (rxConfig()->fpvCamAngleDegrees && IS_RC_MODE_ACTIVE(BOXFPVANGLEMIX) && !FLIGHT_MODE(HEADFREE_MODE)) {
            scaleRcCommandToFpvCamAngle();
        }

        // HEADFREE_MODE in ACRO_MODE
        // yaw rotation is earthframe bound
        if (FLIGHT_MODE(HEADFREE_MODE) && (!FLIGHT_MODE(ANGLE_MODE)) && (!FLIGHT_MODE(HORIZON_MODE))) {
            quaternion  vSetpointRate = VECTOR_INITIALIZE;

            vSetpointRate.x = setpointRate[ROLL];
            vSetpointRate.y = setpointRate[PITCH];
            vSetpointRate.z = setpointRate[YAW];
            quaternionTransformVectorEarthToBody(&vSetpointRate, &qHeadfree);
            setpointRate[ROLL] = constrainf(vSetpointRate.x, -SETPOINT_RATE_LIMIT, SETPOINT_RATE_LIMIT);
            setpointRate[PITCH] = constrainf(vSetpointRate.y, -SETPOINT_RATE_LIMIT, SETPOINT_RATE_LIMIT);
            setpointRate[YAW] = constrainf(vSetpointRate.z, -SETPOINT_RATE_LIMIT, SETPOINT_RATE_LIMIT);
        }

        DEBUG_SET(DEBUG_ANGLERATE, ROLL, setpointRate[ROLL]);
        DEBUG_SET(DEBUG_ANGLERATE, PITCH, setpointRate[PITCH]);
        DEBUG_SET(DEBUG_ANGLERATE, YAW, setpointRate[YAW]);
    }

    if (isRXDataNew) {
        isRXDataNew = false;
    }
}

void updateRcCommands(void)
{
    isRXDataNew = true;
    // rcData is 1000,2000 range, subtract 1000 and clamp between 0 and 1023 (for TPA lookup table indexing)
    uint16_t shift = rcData[THROTTLE] - 1000;
    currentAdjustedThrottle = (shift <= 0) ? 0 : ((shift >= MAX_TPA_THROTTLE) ? MAX_TPA_THROTTLE : shift );
    currentTpaKp = ApplyAttenuationCurve( ((float)currentAdjustedThrottle / MAX_TPA_THROTTLEf), percentCurve[TPA_KP]);
    currentTpaKi = ApplyAttenuationCurve( ((float)currentAdjustedThrottle / MAX_TPA_THROTTLEf), percentCurve[TPA_KI]);
    currentTpaKd = ApplyAttenuationCurve( ((float)currentAdjustedThrottle / MAX_TPA_THROTTLEf), percentCurve[TPA_KD]);
    for (int axis = 0; axis < 3; axis++) {
        // non coupled PID reduction scaler used in PID controller 1 and PID controller 2.

        int32_t tmp = MIN(ABS(rcData[axis] - rxConfig()->midrc), 500);
        if (axis == ROLL || axis == PITCH) {
            if (tmp > rcControlsConfig()->deadband) {
                tmp -= rcControlsConfig()->deadband;
            } else {
                tmp = 0;
            }
            rcCommand[axis] = tmp;
        } else {
            if (tmp > rcControlsConfig()->yaw_deadband) {
                tmp -= rcControlsConfig()->yaw_deadband;
            } else {
                tmp = 0;
            }
            rcCommand[axis] = tmp * -GET_DIRECTION(rcControlsConfig()->yaw_control_reversed);
        }
        if (rcData[axis] < rxConfig()->midrc) {
            rcCommand[axis] = -rcCommand[axis];
        }
    }

    int32_t tmp;
    if (feature(FEATURE_3D)) {
        tmp = constrain(rcData[THROTTLE], PWM_RANGE_MIN, PWM_RANGE_MAX);
        tmp = (uint32_t)(tmp - PWM_RANGE_MIN);
        if (getLowVoltageCutoff()->enabled) {
            tmp = tmp * getLowVoltageCutoff()->percentage / 100;
        }
    } else {
        tmp = constrain(rcData[THROTTLE], rxConfig()->mincheck, PWM_RANGE_MAX);
        tmp = (uint32_t)(tmp - rxConfig()->mincheck) * PWM_RANGE_MIN / (PWM_RANGE_MAX - rxConfig()->mincheck);
        if (getLowVoltageCutoff()->enabled) {
            tmp = tmp * getLowVoltageCutoff()->percentage / 100;
        }
    }

    rcCommand[THROTTLE] = rcLookupThrottle(tmp);

    if (feature(FEATURE_3D) && !failsafeIsActive()) {
        if (!flight3DConfig()->switched_mode3d) {
            if (IS_RC_MODE_ACTIVE(BOX3D)) {
                fix12_t throttleScaler = qConstruct(rcCommand[THROTTLE] - 1000, 1000);
                rcCommand[THROTTLE] = rxConfig()->midrc + qMultiply(throttleScaler, PWM_RANGE_MAX - rxConfig()->midrc);
            }
        } else {
            if (IS_RC_MODE_ACTIVE(BOX3D)) {
                reverseMotors = true;
                fix12_t throttleScaler = qConstruct(rcCommand[THROTTLE] - 1000, 1000);
                rcCommand[THROTTLE] = rxConfig()->midrc + qMultiply(throttleScaler, PWM_RANGE_MIN - rxConfig()->midrc);
            } else {
                reverseMotors = false;
                fix12_t throttleScaler = qConstruct(rcCommand[THROTTLE] - 1000, 1000);
                rcCommand[THROTTLE] = rxConfig()->midrc + qMultiply(throttleScaler, PWM_RANGE_MAX - rxConfig()->midrc);
            }
        }
    }

    // HEADFREE_MODE  in ANGLE_MODE HORIZON_MODE
    // yaw rotation is bodyframe bound
    if (FLIGHT_MODE(HEADFREE_MODE) && (FLIGHT_MODE(ANGLE_MODE) || (FLIGHT_MODE(HORIZON_MODE)))) {
        quaternion  vRcCommand = VECTOR_INITIALIZE;

        vRcCommand.x = rcCommand[ROLL];
        vRcCommand.y = rcCommand[PITCH];
        quaternionTransformVectorEarthToBody(&vRcCommand, &qHeadfree);
        rcCommand[ROLL] = vRcCommand.x;
        rcCommand[PITCH] = vRcCommand.y;
    }
}

void resetYawAxis(void)
{
    rcCommand[YAW] = 0;
    setpointRate[YAW] = 0;
}

bool isMotorsReversed(void)
{
    return reverseMotors;
}

void initRcProcessing(void)
{
    for (int i = 0; i < THROTTLE_LOOKUP_LENGTH; i++) {
        const int16_t tmp = 10 * i - currentControlRateProfile->thrMid8;
        uint8_t y = 1;
        if (tmp > 0)
            y = 100 - currentControlRateProfile->thrMid8;
        if (tmp < 0)
            y = currentControlRateProfile->thrMid8;
        lookupThrottleRC[i] = 10 * currentControlRateProfile->thrMid8 + tmp * (100 - currentControlRateProfile->thrExpo8 + (int32_t) currentControlRateProfile->thrExpo8 * (tmp * tmp) / (y * y)) / 10;
        lookupThrottleRC[i] = PWM_RANGE_MIN + (PWM_RANGE_MAX - PWM_RANGE_MIN) * lookupThrottleRC[i] / 1000; // [MINTHROTTLE;MAXTHROTTLE]
    }

    switch (currentControlRateProfile->rates_type) {
    case RATES_TYPE_BETAFLIGHT:
    default:
        applyRates = applyBetaflightRates;

        break;
    case RATES_TYPE_RACEFLIGHT:
        applyRates = applyRaceFlightRates;

        break;
    }
    interpolationChannels = rxConfig()->rcInterpolationChannels + 2; //"RP", "RPY", "RPYT"
    BuildTPACurve();
}
