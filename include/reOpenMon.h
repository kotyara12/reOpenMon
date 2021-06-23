/* 
   EN: Module for sending data to open-monitoring.online from ESP32
   RU: Модуль для отправки данных на open-monitoring.online из ESP32
   --------------------------
   (с) 2020-2021 Разживин Александр | Razzhivin Alexander
   kotyara12@yandex.ru | https://kotyara12.ru | tg: @kotyara1971
*/

#ifndef __RE_OPENMON_H__
#define __RE_OPENMON_H__

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

/**
 * EN: Task management: create, suspend, resume and delete
 * RU: Управление задачей: создание, приостановка, восстановление и удаление
 **/
bool omTaskCreate();
bool omTaskSuspend();
bool omTaskResume();
bool omTaskDelete();

/**
 * EN: Controller initialization
 * RU: Создание контроллера.
 * 
 * @param om_id - Controller ID / Идентификатор контроллера
 * @param om_key - Controller token / Токен контроллера
 **/
om_ctrl_t * omInitController(const uint32_t om_id, const char * om_key);
/**
 * EN: Removing a controller and free resources
 * RU: Удаление контроллера и освобождение ресурсов
 **/
void omFreeController(om_ctrl_t * omController);

/**
 * EN: Sending data to the specified controller. The fields string will be removed after submission.
 * If little time has passed since the last data sent to the controller, the data will be queued.
 * If there is already data in the queue for this controller, it will be overwritten with new data.
 * 
 * RU: Отправка данных в заданный контроллер. Строка fields будет удалена после отправки. 
 * Если с момента последней отправки данных в контроллер прошло мало времени, то данные будут поставлены в очередь.
 * Если в очереди на данный контроллер уже есть данные, то они будут перезаписаны новыми данными.
 * 
 * @param omController - Pointer to controller from omInitController / Указатель на контроллер из omInitController 
 * @param fields - Data in the format p1=... / Данные в формате p1=...
 **/
bool omSend(om_ctrl_t * omController, char * fields);

#ifdef __cplusplus
}
#endif

#endif // __RE_OPENMON_H__
