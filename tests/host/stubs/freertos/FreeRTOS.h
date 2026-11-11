#pragma once
#include <stdint.h>
typedef unsigned TickType_t;
typedef int BaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(x) (x)
#define portYIELD_FROM_ISR(x) ((void)(x))
