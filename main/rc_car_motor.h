#pragma once

#include <stdbool.h>

typedef enum {
    kRcCarMotorCmdStop = 0,
    kRcCarMotorCmdForward,
    kRcCarMotorCmdBackward,
    kRcCarMotorCmdLeft,
    kRcCarMotorCmdRight,
} RcCarMotorCmd;

bool RcCarMotor_Init(void);
void RcCarMotor_Deinit(void);
void RcCarMotor_Apply(RcCarMotorCmd cmd);
const char* RcCarMotor_CommandName(RcCarMotorCmd cmd);
