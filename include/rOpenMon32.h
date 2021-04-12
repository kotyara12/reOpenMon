/* 
   EN: Module for sending data to open-monitoring.online from ESP32
   RU: Модуль для отправки данных на open-monitoring.online из ESP32
   --------------------------
   (с) 2020-2021 Разживин Александр | Razzhivin Alexander
   kotyara12@yandex.ru | https://kotyara12.ru | tg: @kotyara1971
*/

#ifndef __ROPENMON32_H__
#define __ROPENMON32_H__

#include <stddef.h>
#include <sys/types.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h" 
#include "freertos/semphr.h" 
#include "project_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OPENMON_TOKEN_LENGTH 6

typedef struct {
  uint32_t id;
  char key[OPENMON_TOKEN_LENGTH];
  unsigned long last_send;
  unsigned long next_send;
  xSemaphoreHandle lock;
  char* data;
} om_ctrl_t;


bool omTaskCreate();
bool omTaskSuspend();
bool omTaskResume();
bool omTaskDelete();
om_ctrl_t * omInitController(const uint32_t om_id, const char * om_key);
void omFreeController(om_ctrl_t * omController);
bool omSend(om_ctrl_t * omController, char * fields);

#ifdef __cplusplus
}
#endif

#endif // __ROPENMON32_H__
