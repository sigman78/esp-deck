#pragma once
#include <stdint.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef uint32_t  TickType_t;
typedef int       BaseType_t;
typedef HANDLE    TaskHandle_t;

#define pdPASS         ((BaseType_t)1)
#define pdFAIL         ((BaseType_t)0)
#define portMAX_DELAY  ((TickType_t)0xFFFFFFFFUL)
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
