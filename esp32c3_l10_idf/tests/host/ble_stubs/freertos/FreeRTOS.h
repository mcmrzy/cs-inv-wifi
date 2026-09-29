#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define pdTRUE 1
#define pdFALSE 0
#define pdPASS pdTRUE
#define pdMS_TO_TICKS(ms) (ms)

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
typedef void *TaskHandle_t;
