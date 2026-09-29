#pragma once
#include "FreeRTOS.h"

typedef struct HostQueue *QueueHandle_t;

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size);
void xQueueReset(QueueHandle_t queue);
void vQueueDelete(QueueHandle_t queue);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue);
