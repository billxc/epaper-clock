#pragma once
#include <pthread.h>
typedef pthread_mutex_t *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t mutex, int wait);
int xSemaphoreGive(SemaphoreHandle_t mutex);
