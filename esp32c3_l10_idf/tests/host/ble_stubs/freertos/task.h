#pragma once
#include "FreeRTOS.h"

typedef void (*TaskFunction_t)(void *);
typedef int (*HostTaskFn)(void *);

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack,
                       void *arg, UBaseType_t prio, TaskHandle_t *out);
void vTaskDelete(TaskHandle_t handle);
void vTaskDelay(TickType_t ticks);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t handle);

/* Host test helpers: run the pending worker once, then clear its handle. */
void host_freertos_run_pending_task(void);
void host_freertos_reset(void);
