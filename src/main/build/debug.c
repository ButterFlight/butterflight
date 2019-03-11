#include "stdint.h"


#include "debug.h"

int16_t debug[DEBUG16_VALUE_COUNT];
uint8_t debugMode;

#ifdef DEBUG_SECTION_TIMES
uint32_t sectionTimes[2][4];
#endif

const char * const debugModeNames[DEBUG_COUNT] = {
    "NONE",
    "CYCLETIME",
    "BATTERY",
    "GYRO_FILTER",
    "GYRO_FILTER_DIFF",
    "ACC_RAW",
    "ACC",
    "PIDLOOP",
    "GYRO_SCALED",
    "RC_INTERPOLATION",
    "ANGLERATE",
    "ESC_SENSOR",
    "SCHEDULER",
    "STACK",
    "ESC_SENSOR_RPM",
    "ESC_SENSOR_TMP",
    "ALTITUDE",
    "DYNAMIC_NOTCH",
    "DYNAMIC_NOTCH_TIME",
    "DYNAMIC_NOTCH_FREQ",
    "RX_FRSKY_SPI",
    "GYRO_RAW",
    "DUAL_GYRO",
    "DUAL_GYRO_RAW",
    "DUAL_GYRO_COMBINE",
    "DUAL_GYRO_DIFF",
    "MAX7456_SIGNAL",
    "MAX7456_SPICLOCK",
    "SBUS",
    "FPORT",
    "RANGEFINDER",
    "RANGEFINDER_QUALITY",
    "LIDAR_TF",
    "CORE_TEMP",
    "RUNAWAY_TAKEOFF",
    "SDIO",
    "CURRENT_SENSOR",
    "USB",
    "SMARTAUDIO",
    "RTH",
    "ITERM_RELAX",
    "ACRO_TRAINER",
    "RC_SMOOTHING",
    "RX_SIGNAL_LOSS",
    "RC_SMOOTHING_RATE",
    "ANTI_GRAVITY",
    "IMU",
    "DTERM_FILTER_DIFF",
    "KALMAN",
};
