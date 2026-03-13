#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool RcCarCamera_Init(void);
void RcCarCamera_Deinit(void);
bool RcCarCamera_IsReady(void);
const char* RcCarCamera_LastError(void);
