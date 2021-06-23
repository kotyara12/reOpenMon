#include "reOpenMon.h"
#include "project_config.h"
#include <cstring>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include "rLog.h"
#include "rStrings.h"
#include "esp_http_client.h"
#include "reEsp32.h"
#include "reLed.h"
#include "reLedSys.h"
#include "rePing.h"
#include "reWiFi.h"
#include "sys/queue.h"

#define API_OPENMON_HOST "open-monitoring.online"
#define API_OPENMON_PORT 80
#define API_OPENMON_SEND_PATH "/get"
#define API_OPENMON_SEND_VALUES "cid=%d&key=%s&%s"

typedef struct {
  uint32_t id;
  char key[OPENMON_TOKEN_LENGTH];
  time_t last_send;
  char* data;
  SLIST_ENTRY(omController_t) next;
} omController_t;
typedef struct omController_t *omControllerHandle_t;

SLIST_HEAD(omHead_t, omController_t);
typedef struct omHead_t *omHeadHandle_t;

typedef struct {
  uint32_t id;
  char* data;
} omQueueItem_t;  

#define OPENMON_QUEUE_ITEM_SIZE sizeof(omQueueItem_t*)

TaskHandle_t _omTask;
QueueHandle_t _omQueue = nullptr;
omHeadHandle_t _omControllers = nullptr;

static const char* tagOM = "OpenMon";
static const char* omTaskName = "openMon";

#if CONFIG_OPENMON_STATIC_ALLOCATION
StaticQueue_t _omQueueBuffer;
StaticTask_t _omTaskBuffer;
StackType_t _omTaskStack[CONFIG_OPENMON_STACK_SIZE];
uint8_t _omQueueStorage [CONFIG_OPENMON_QUEUE_SIZE * OPENMON_QUEUE_ITEM_SIZE];
#endif // CONFIG_OPENMON_STATIC_ALLOCATION

bool omSendFailed = false;

bool omSendEx(om_ctrl_t * omController)
{
  bool _result = true;
  char* get_request = nullptr;

  // Block data from changing
  do { vPortYield(); } 
  while (xSemaphoreTake(omController->lock, portMAX_DELAY) != pdPASS);
  // Create the text of the GET request
  if (omController->data) {
    get_request = malloc_stringf(API_OPENMON_SEND_VALUES, omController->id, omController->key, omController->data);
    // Raw data is no longer needed
    free(omController->data);
    omController->data = nullptr;
  };
  // Remove the lock
  xSemaphoreGive(omController->lock);

  if (get_request) {
    ledSysOn(true);
    // Configuring request parameters
    esp_http_client_config_t cfgHttp;
    memset(&cfgHttp, 0, sizeof(cfgHttp));
    cfgHttp.method = HTTP_METHOD_GET;
    cfgHttp.host = API_OPENMON_HOST;
    cfgHttp.port = API_OPENMON_PORT;
    cfgHttp.path = API_OPENMON_SEND_PATH;
    cfgHttp.query = get_request;
    cfgHttp.use_global_ca_store = false;
    cfgHttp.transport_type = HTTP_TRANSPORT_OVER_TCP;
    cfgHttp.is_async = false;
    // Make a request to the API
    esp_http_client_handle_t client = esp_http_client_init(&cfgHttp);
    if (client != NULL) {
      esp_err_t err = esp_http_client_perform(client);
      if (err == ESP_OK) {
        int retCode = esp_http_client_get_status_code(client);
        _result = ((retCode == 200) || (retCode == 301));
        if (_result)
          rlog_i(tagOM, "Data sent: %s", get_request);
        else 
          rlog_e(tagOM, "Failed to send message, API error code: #%d!", retCode);
      }
      else {
        _result = false;
        rlog_e(tagOM, "Failed to complete request to open-monitoring.online, error code: 0x%x!", err);
      };
      esp_http_client_cleanup(client);
    }
    else {
      _result = false;
      rlog_e(tagOM, "Failed to complete request to open-monitoring.online!");
    };
    ledSysOff(true);
    // We fix the time of the last access to the server
    omController->last_send = millis();
  };
  // Remove the request from memory
  if (get_request) free(get_request);
  return _result;
}

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------- Controller list -----------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool omControllersInit()
{
  _omControllers = new omHead_t;
  if (_omControllers) {
    SLIST_INIT(_omControllers);
  };
  return (_omControllers);
}

omControllerHandle_t omControllerFind(uint32_t id)
{
  mqttPubHandle_t item;
  SLIST_FOREACH(item, outbox, next) {
    if (strcasecmp(item->topic, msg->topic) == 0) {
      return item;
    }
  }
  return NULL;
} 

bool mqttOutboxPublish(mqttOutboxHandle_t outbox)
{
  mqttPubHandle_t item;
  item = STAILQ_FIRST(outbox);
  if (item) {
    rlog_v(tagMQTTQ, "Processing outbox message \"%s\" [ %d ]", item->topic, strlen(item->payload));
    ledSysOn(true);
    bool send = false;
    if (item->payload) {
      send = esp_mqtt_publish(item->topic, item->payload, item->qos, item->retained);
    } else {
      send = esp_mqtt_publish(item->topic, "", item->qos, item->retained);
    };
    ledSysOff(true);
    if (send) {
      // Removing a message from the send queue
      STAILQ_REMOVE(outbox, item, mqttPub_t, next);
      // Removing a message from heap
      if ((item->payload) && (item->free_payload)) free(item->payload);
      if (item->free_topic) free(item->topic);
      delete item;
      // Resetting the MQTT error status if it was
      ledSysStateClear(SYSLED_MQTT_ERROR, false);
    } else {
      // Failed to send message, set the error flag
      ledSysStateSet(SYSLED_MQTT_ERROR, false);
      // Reducing the number of sending attempts
      item->remain_cnt--;
      if (item->remain_cnt > 0) {
        rlog_e(tagMQTTQ, "Failed send message \"%s\" [ %d ]", item->topic, strlen(item->payload));
      } else {
        rlog_e(tagMQTTQ, "Failed send message \"%s\" [ %d ], message lost", item->topic, strlen(item->payload));
        // Removing a message from the send queue
        STAILQ_REMOVE(outbox, item, mqttPub_t, next);
        // Removing a message from heap
        if (item->free_payload) free(item->payload);
        if (item->free_topic) free(item->topic);
        delete item;
      };
      return false;
    };
    // esp_task_wdt_reset();
    // taskYIELD();
    vTaskDelay(1);
  };
  return true;  
}

void mqttOutboxFree(mqttOutboxHandle_t outbox)
{
  mqttPubHandle_t item, tmp;
  STAILQ_FOREACH_SAFE(item, outbox, next, tmp) {
    // Removing a message from the send queue
    STAILQ_REMOVE(outbox, item, mqttPub_t, next);
    // Removing a message from heap
    if (item->free_payload) free(item->payload);
    if (item->free_topic) free(item->topic);
    delete item;
  };
  delete outbox;
}



om_ctrl_t * omInitController(const uint32_t om_id, const char * om_key)
{
  om_ctrl_t * ret = new om_ctrl_t;
  if (ret == NULL) {
    rlog_e(tagOM, "Memory allocation error for data structure!");
    return NULL;
  };

  ret->id = om_id;
  strcpy(ret->key, om_key);
  ret->last_send = 0;
  ret->next_send = 0;
  ret->data = nullptr;
  
  ret->lock = xSemaphoreCreateMutex();
  if (ret->lock == NULL) {
    rlog_e(tagOM, "Error creating data lock mutex!");
    delete ret;
    return NULL;
  };

  return ret;
}

void omFreeController(om_ctrl_t * omController)
{
  if (omController) {
    if (omController->lock) vSemaphoreDelete(omController->lock);
    if (omController->data) free(omController->data);
    delete omController;
  };
}

bool omSend(om_ctrl_t * omController, char * fields)
{
  if ((_omQueue) && (omController) && (omController->lock) && (fields)) {
    // Blocking data from changing
    if (xSemaphoreTake(omController->lock, CONFIG_OPENMON_QUEUE_WAIT / portTICK_RATE_MS) == pdPASS) {
      // If there was any data in the send queue, delete it
      if (omController->data) free(omController->data);
      omController->data = fields;
      // We set the time of sending data of this controller
      if (checkTimeout(omController->last_send, CONFIG_OPENMON_MIN_INTERVAL)) {
        omController->next_send = 0;
      }
      else {
        omController->next_send = omController->last_send + CONFIG_OPENMON_MIN_INTERVAL;
      };
      // Unlocking data
      xSemaphoreGive(omController->lock);

      // We add a message to the queue so as not to delay the calling thread in case of problems with sending
      if (xQueueSend(_omQueue, &omController, CONFIG_OPENMON_QUEUE_WAIT / portTICK_RATE_MS) == pdPASS) {
        return true;
      }
      else {
        rloga_e("Error adding message to queue [ %s ]!", omTaskName);
        ledSysStateSet(SYSLED_OTHER_PUB_ERROR, false);
      };
    }
    else {
      rloga_e("Failed to access the send queue [ %s ]!", omTaskName);
      ledSysStateSet(SYSLED_OTHER_PUB_ERROR, false);
    };
  };

  return false;
}

void omProcessQueue(TickType_t xTicksToWait)
{
  om_ctrl_t * omController;
 
  if (xQueueReceive(_omQueue, &omController, xTicksToWait) == pdPASS) {
    // If it is a delayed dispatch, we wait for the start time
    while (omController->next_send > millis()) {
      // While we are waiting, we recursively process other controllers, if they are in the queue.
      omProcessQueue(1);
    };

    // Trying to send a message to the OpenMonitoring API
    uint16_t tryAttempt = 1;
    bool resAttempt = false;
    do {
      // Checking Internet and host availability
      checkHost(API_OPENMON_HOST, tryAttempt > 1, tagOM, SYSLED_OTHER_PUB_ERROR, CONFIG_MESSAGE_TG_HOST_AVAILABLE, CONFIG_MESSAGE_TG_HOST_UNAVAILABLE, 
        CONFIG_HOST_PING_SESSION_COUNT, CONFIG_HOST_PING_SESSION_INTERVAL, CONFIG_HOST_PING_SESSION_TIMEOUT, CONFIG_HOST_PING_SESSION_DATASIZE);

      // Trying to send a message to the OpenMonitoring API
      resAttempt = omSendEx(omController);
      if (resAttempt) {
        if (omSendFailed) {
          omSendFailed = false;
          ledSysStateClear(SYSLED_OTHER_PUB_ERROR, false);
        };
      }
      else {
        if (!omSendFailed) {
          omSendFailed = true;
          ledSysStateSet(SYSLED_OTHER_PUB_ERROR, false);
          tryAttempt++;
          vTaskDelay(CONFIG_OPENMON_MIN_INTERVAL / portTICK_RATE_MS);
        };
      };
      // esp_task_wdt_reset();
    } while (!resAttempt && (tryAttempt <= CONFIG_OPENMON_MAX_ATTEMPTS));
  };
}

void omTaskExec(void *pvParameters)
{
  while (true) {
    // We are waiting for an Internet connection, if for some reason it is not (and you can also pause the task if the connection is interrupted, but this is not necessary)
    if (!wifiIsConnected()) {
      ledSysStateSet(SYSLED_OTHER_PUB_ERROR, false);
      wifiWaitConnection(portMAX_DELAY);
      ledSysStateClear(SYSLED_OTHER_PUB_ERROR, false);
    };

    // Processing the dispatch queue
    omProcessQueue(portMAX_DELAY);
  };

  omTaskDelete();
}

bool omTaskSuspend()
{
  if ((_omTask != NULL) && (eTaskGetState(_omTask) != eSuspended)) {
    vTaskSuspend(_omTask);
    rloga_d("Task [ %s ] has been successfully suspended", omTaskName);
    return true;
  }
  else {
    rloga_w("Task [ %s ] not found or is already suspended", omTaskName);
    return false;
  };
}

bool omTaskResume()
{
  if ((_omTask != NULL) && wifiIsConnected() && (eTaskGetState(_omTask) == eSuspended)) {
    vTaskResume(_omTask);
    rloga_d("Task [ %s ] has been successfully started", omTaskName);
    return true;
  }
  else {
    rloga_w("Task [ %s ] is not found or is already running", omTaskName);
    return false;
  };
}

bool omTaskCreate() 
{
  if (!_omTask) {
    if (!_omControllers) {
      if !(omControllersInit()) {
        rloga_e("Failed to create a list of controllers!");
        ledSysStateSet(SYSLED_ERROR, false);
        return false;
      };
    };

    if (!_omQueue) {
      #if CONFIG_OPENMON_STATIC_ALLOCATION
      _omQueue = xQueueCreateStatic(CONFIG_OPENMON_QUEUE_SIZE, OPENMON_QUEUE_ITEM_SIZE, &(_omQueueStorage[0]), &_omQueueBuffer);
      #else
      _omQueue = xQueueCreate(CONFIG_OPENMON_QUEUE_SIZE, OPENMON_QUEUE_ITEM_SIZE);
      #endif // CONFIG_OPENMON_STATIC_ALLOCATION
      if (!_omQueue) {
        rloga_e("Failed to create a queue for sending data to OpenMonitoring!");
        ledSysStateSet(SYSLED_ERROR, false);
        return false;
      };
    };
    
    #if CONFIG_OPENMON_STATIC_ALLOCATION
    _omTask = xTaskCreateStaticPinnedToCore(omTaskExec, omTaskName, CONFIG_OPENMON_STACK_SIZE, NULL, CONFIG_OPENMON_PRIORITY, _omTaskStack, &_omTaskBuffer, CONFIG_OPENMON_CORE); 
    #else
    xTaskCreatePinnedToCore(omTaskExec, omTaskName, CONFIG_OPENMON_STACK_SIZE, NULL, CONFIG_OPENMON_PRIORITY, &_omTask, CONFIG_OPENMON_CORE); 
    #endif // CONFIG_OPENMON_STATIC_ALLOCATION
    if (_omTask == NULL) {
      vQueueDelete(_omQueue);
      rloga_e("Failed to create task for sending data to OpenMonitoring!");
      ledSysStateSet(SYSLED_ERROR, false);
      return false;
    }
    else {
      rloga_d("Task [ %s ] has been successfully started", omTaskName);
      ledSysStateClear(SYSLED_OTHER_PUB_ERROR, false);
      return true;
    };
  }
  else {
    return omTaskResume();
  };
}

bool omTaskDelete()
{
  if (_omQueue != NULL) {
    vQueueDelete(_omQueue);
    _omQueue = NULL;
  };

  if (_omTask != NULL) {
    vTaskDelete(_omTask);
    _omTask = NULL;
    rloga_d("Task [ %s ] was deleted", omTaskName);
  };
  
  return true;
}

