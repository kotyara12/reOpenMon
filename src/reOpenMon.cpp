#include "project_config.h"

#if CONFIG_OPENMON_ENABLE

#include "reOpenMon.h"
#include <cstring>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include "rLog.h"
#include "rTypes.h"
#include "rStrings.h"
#include "esp_http_client.h"
#include "reWiFi.h"
#include "reEvents.h"
#include "reLed.h"
#include "reStates.h"
#include "reEsp32.h"
#include "sys/queue.h"
#include "def_consts.h"

#define API_OPENMON_HOST "open-monitoring.online"
#define API_OPENMON_PORT 443
#define API_OPENMON_SEND_PATH "/get"
#define API_OPENMON_SEND_VALUES "cid=%d&key=%s&%s"
#define API_OPENMON_TIMEOUT_MS 30000

typedef enum {
  OM_OK         = 0,
  OM_ERROR_API  = 1,
  OM_ERROR_HTTP = 2
} omSendStatus_t;

typedef struct omController_t {
  uint32_t id;
  const char* key;
  uint32_t interval;
  TickType_t next_send;
  uint8_t attempt;
  char* fields;
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

static const char* logTAG = "OpMn";
static const char* omTaskName = "open_mon";

#if CONFIG_OPENMON_STATIC_ALLOCATION
StaticQueue_t _omQueueBuffer;
StaticTask_t _omTaskBuffer;
StackType_t _omTaskStack[CONFIG_OPENMON_STACK_SIZE];
uint8_t _omQueueStorage [CONFIG_OPENMON_QUEUE_SIZE * OPENMON_QUEUE_ITEM_SIZE];
#endif // CONFIG_OPENMON_STATIC_ALLOCATION

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------- Controller list -----------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool omControllersInit()
{
  _omControllers = (omHead_t*)esp_calloc(1, sizeof(omHead_t));
  if (_omControllers) {
    SLIST_INIT(_omControllers);
  };
  return (_omControllers);
}

bool omControllerInit(const uint32_t omId, const char * omKey, const uint32_t omInterval)
{
  if (!_omControllers) {
    omControllersInit();
  };

  if (!_omControllers) {
    rlog_e(logTAG, "The controller list has not been initialized!");
    return false;
  };
    
  omControllerHandle_t ctrl = (omController_t*)esp_calloc(1, sizeof(omController_t));
  if (!ctrl) {
    rlog_e(logTAG, "Memory allocation error for data structure!");
    return false;
  };

  ctrl->id = omId;
  ctrl->key = omKey;
  if (omInterval < CONFIG_OPENMON_MIN_INTERVAL) {
    ctrl->interval = pdMS_TO_TICKS(CONFIG_OPENMON_MIN_INTERVAL);
  } else {
    ctrl->interval = pdMS_TO_TICKS(omInterval);
  };
  ctrl->next_send = 0;
  ctrl->attempt = 0;
  ctrl->fields = nullptr;
  SLIST_NEXT(ctrl, next) = nullptr;
  SLIST_INSERT_HEAD(_omControllers, ctrl, next);
  return true;
}

omControllerHandle_t omControllerFind(const uint32_t omId)
{
  omControllerHandle_t item;
  SLIST_FOREACH(item, _omControllers, next) {
    if (item->id == omId) {
      return item;
    }
  }
  return nullptr;
} 

void omControllersFree()
{
  omControllerHandle_t item, tmp;
  SLIST_FOREACH_SAFE(item, _omControllers, next, tmp) {
    SLIST_REMOVE(_omControllers, item, omController_t, next);
    if (item->fields) free(item->fields);
    free(item);
  };
  free(_omControllers);
}

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------ Adding data to the send queue ----------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool omSend(const uint32_t omId, char * omFields)
{
  if (_omQueue) {
    omQueueItem_t* item = (omQueueItem_t*)esp_calloc(1, sizeof(omQueueItem_t));
    if (item) {
      item->id = omId;
      item->data = omFields;
      if (xQueueSend(_omQueue, &item, CONFIG_OPENMON_QUEUE_WAIT / portTICK_RATE_MS) == pdPASS) {
        return true;
      };
    };
    rloga_e("Error adding message to queue [ %s ]!", omTaskName);
    eventLoopPostSystem(RE_SYS_OPENMON_ERROR, RE_SYS_SET, false);
  };
  return false;
}

// -----------------------------------------------------------------------------------------------------------------------
// ----------------------------------------------------- Call API --------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

omSendStatus_t omSendEx(const omControllerHandle_t ctrl)
{
  omSendStatus_t _result = OM_OK;
  char* get_request = nullptr;

  // Create the text of the GET request
  if (ctrl->fields) {
    get_request = malloc_stringf(API_OPENMON_SEND_VALUES, ctrl->id, ctrl->key, ctrl->fields);
  };

  if (get_request) {
    // Configuring request parameters
    esp_http_client_config_t cfgHttp;
    memset(&cfgHttp, 0, sizeof(cfgHttp));
    cfgHttp.method = HTTP_METHOD_GET;
    cfgHttp.host = API_OPENMON_HOST;
    cfgHttp.port = API_OPENMON_PORT;
    cfgHttp.path = API_OPENMON_SEND_PATH;
    cfgHttp.timeout_ms = API_OPENMON_TIMEOUT_MS;
    cfgHttp.query = get_request;
    cfgHttp.use_global_ca_store = true;
    cfgHttp.transport_type = HTTP_TRANSPORT_OVER_SSL;
    cfgHttp.is_async = false;

    // Make a request to the API
    esp_http_client_handle_t client = esp_http_client_init(&cfgHttp);
    if (client != NULL) {
      esp_err_t err = esp_http_client_perform(client);
      if (err == ESP_OK) {
        int retCode = esp_http_client_get_status_code(client);
        if ((retCode >= HttpStatus_Ok) && (retCode <= HttpStatus_BadRequest)) {
          _result = OM_OK;
          rlog_i(logTAG, "Data sent # %d: %s", ctrl->id, get_request);
        } else {
          _result = OM_ERROR_API;
          rlog_e(logTAG, "Failed to send message, API error code: #%d!", retCode);
        };
        // Flashing system LED
        #if CONFIG_SYSLED_SEND_ACTIVITY
        ledSysActivity();
        #endif // CONFIG_SYSLED_SEND_ACTIVITY
      }
      else {
        _result = OM_ERROR_HTTP;
        rlog_e(logTAG, "Failed to complete request to open-monitoring.online, error code: 0x%x!", err);
      };
      esp_http_client_cleanup(client);
    }
    else {
      _result = OM_ERROR_HTTP;
      rlog_e(logTAG, "Failed to complete request to open-monitoring.online!");
    };
  };
  // Remove the request from memory
  if (get_request) free(get_request);
  return _result;
}

// -----------------------------------------------------------------------------------------------------------------------
// --------------------------------------------------- Queue processing --------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

void omTaskExec(void *pvParameters)
{
  omQueueItem_t * item = nullptr;
  omControllerHandle_t ctrl = nullptr;
  TickType_t wait_queue = portMAX_DELAY;
  static omSendStatus_t send_status = OM_ERROR_HTTP;
  static uint32_t send_errors = 0;
  static time_t time_first_error = 0;
  
  while (true) {
    // Receiving new data
    if (xQueueReceive(_omQueue, &item, wait_queue) == pdPASS) {
      ctrl = omControllerFind(item->id);
      if (ctrl) {
        // Replacing controller data with new ones from the transporter
        ctrl->attempt = 0;
        if (ctrl->fields) free(ctrl->fields);
        ctrl->fields = item->data;
        item->data = nullptr;
      } else {
        rlog_e(logTAG, "Controller # %d not found!", item->id);
      };
      // Free transporter
      if (item->data) free(item->data);
      free(item);
      item = nullptr;
    };

    // Check internet availability 
    if (statesInetIsAvailabled()) {
      ctrl = nullptr;
      wait_queue = portMAX_DELAY;
      SLIST_FOREACH(ctrl, _omControllers, next) {
        // Sending data
        if (ctrl->fields) {
          if (ctrl->next_send < xTaskGetTickCount()) {
            // Attempt to send data
            ctrl->attempt++;
            send_status = omSendEx(ctrl);
            if (send_status == OM_OK) {
              // Calculate the time of the next dispatch in the given controller
              ctrl->next_send = xTaskGetTickCount() + ctrl->interval;
              ctrl->attempt = 0;
              if (ctrl->fields) {
                free(ctrl->fields);
                ctrl->fields = nullptr;
              };
              // If the error counter exceeds the threshold, then a notification has been sent - send a recovery notification
              if (send_errors >= CONFIG_OPENMON_ERROR_LIMIT) {
                eventLoopPostSystem(RE_SYS_OPENMON_ERROR, RE_SYS_CLEAR, false, time_first_error);
              };
              time_first_error = 0;
              send_errors = 0;
            } else {
              // Increase the number of errors in a row and fix the time of the first error
              send_errors++;
              if (time_first_error == 0) {
                time_first_error = time(nullptr);
              };
              // Calculate the time of the next dispatch in the given controller
              if (ctrl->attempt < CONFIG_OPENMON_MAX_ATTEMPTS) {
                ctrl->next_send = xTaskGetTickCount() + pdMS_TO_TICKS(CONFIG_OPENMON_ERROR_INTERVAL);
              } else {
                if (ctrl->fields) {
                  free(ctrl->fields);
                  ctrl->fields = nullptr;
                };
                ctrl->next_send = xTaskGetTickCount() + ctrl->interval;
                ctrl->attempt = 0;
                rlog_e(logTAG, "Failed to send data to controller #%d!", ctrl->id);
              };
              // If the error counter has reached the threshold, send a notification
              if (send_errors == CONFIG_OPENMON_ERROR_LIMIT) {
                eventLoopPostSystem(RE_SYS_OPENMON_ERROR, RE_SYS_SET, false, time_first_error);
              };
            };
          };

          // Find the minimum delay before the next sending to the controller
          if (ctrl->fields) {
            TickType_t send_delay = 0;
            if (ctrl->next_send > xTaskGetTickCount()) {
              send_delay = ctrl->next_send - xTaskGetTickCount();
            };
            if (send_delay > ctrl->interval) {
              ctrl->next_send = xTaskGetTickCount() + ctrl->interval;
              send_delay = ctrl->interval;
            };
            if (send_delay < wait_queue) {
              wait_queue = send_delay;
            };
          };
        };
      };
    } else {
      // If the Internet is not available, repeat the check every second
      wait_queue = pdMS_TO_TICKS(1000); 
    };
  };
  omTaskDelete();
}

// -----------------------------------------------------------------------------------------------------------------------
// -------------------------------------------------- Task routines ------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool omTaskSuspend()
{
  if ((_omTask) && (eTaskGetState(_omTask) != eSuspended)) {
    vTaskSuspend(_omTask);
    if (eTaskGetState(_omTask) == eSuspended) {
      rloga_d("Task [ %s ] has been suspended", omTaskName);
      return true;
    } else {
      rloga_e("Failed to suspend task [ %s ]!", omTaskName);
    };
  };
  return false;  
}

bool omTaskResume()
{
  if ((_omTask) && (eTaskGetState(_omTask) == eSuspended)) {
    vTaskResume(_omTask);
    if (eTaskGetState(_omTask) != eSuspended) {
      rloga_i("Task [ %s ] has been successfully resumed", omTaskName);
      return true;
    } else {
      rloga_e("Failed to resume task [ %s ]!", omTaskName);
    };
  };
  return false;  
}

bool omTaskCreate(bool createSuspended) 
{
  if (!_omTask) {
    if (!_omControllers) {
      if (!omControllersInit()) {
        rloga_e("Failed to create a list of controllers!");
        eventLoopPostSystem(RE_SYS_ERROR, RE_SYS_SET, false);
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
        omControllersFree();
        rloga_e("Failed to create a queue for sending data to OpenMonitoring!");
        eventLoopPostSystem(RE_SYS_ERROR, RE_SYS_SET, false);
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
      omControllersFree();
      rloga_e("Failed to create task for sending data to OpenMonitoring!");
      eventLoopPostSystem(RE_SYS_ERROR, RE_SYS_SET, false);
      return false;
    }
    else {
      if (createSuspended) {
        rloga_i("Task [ %s ] has been successfully created", omTaskName);
        omTaskSuspend();
        eventLoopPostSystem(RE_SYS_OPENMON_ERROR, RE_SYS_SET, false);
      } else {
        rloga_i("Task [ %s ] has been successfully started", omTaskName);
        eventLoopPostSystem(RE_SYS_OPENMON_ERROR, RE_SYS_CLEAR, false);
      };
      return omEventHandlerRegister();
    };
  };
  return false;
}

bool omTaskDelete()
{
  omControllersFree();

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

// -----------------------------------------------------------------------------------------------------------------------
// --------------------------------------------------- Events handlers ---------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

static void omWiFiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  if (event_id == RE_WIFI_STA_PING_OK) {
    omTaskResume();
  };
}

static void omOtaEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  if ((event_id == RE_SYS_OTA) && (event_data)) {
    re_system_event_data_t* data = (re_system_event_data_t*)event_data;
    if (data->type == RE_SYS_SET) {
      omTaskSuspend();
    } else {
      omTaskResume();
    };
  };
}

bool omEventHandlerRegister()
{
  return eventHandlerRegister(RE_WIFI_EVENTS, RE_WIFI_STA_PING_OK, &omWiFiEventHandler, nullptr)
      && eventHandlerRegister(RE_SYSTEM_EVENTS, RE_SYS_OTA, &omOtaEventHandler, nullptr);
};

#endif // CONFIG_OPENMON_ENABLE
