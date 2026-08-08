#ifndef STUB_FREERTOS_H
#define STUB_FREERTOS_H
typedef int BaseType_t;
typedef void *TaskHandle_t;
#define pdTRUE 1
#define pdFALSE 0
#define portMUX_INITIALIZER_UNLOCKED {0}
typedef struct { int dummy; } portMUX_TYPE;
#define portENTER_CRITICAL(m)
#define portEXIT_CRITICAL(m)
#define xTaskCreate(...) 0
#define xTaskNotifyGive(h)
#define ulTaskNotifyTake(a, b) 0
#define portMAX_DELAY 0
#endif
