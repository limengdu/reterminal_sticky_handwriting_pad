#pragma once
#include "FreeRTOS.h"
typedef void *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateBinary(void);
void vSemaphoreDelete(SemaphoreHandle_t);
int xSemaphoreTake(SemaphoreHandle_t,TickType_t);
int xSemaphoreGiveFromISR(SemaphoreHandle_t,BaseType_t *);
