#include "reOpenMon.h"
#include "project_config.h"
#include <cstring>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include "rLog.h"
#include "rTypes.h"
#include "rStrings.h"
#include "esp_http_client.h"
#include "reEsp32.h"
#include "reLed.h"
#include "reLedSys.h"
#include "rePing.h"
#include "reWiFi.h"
#include "sys/queue.h"
#if CONFIG_TELEGRAM_ENABLE
#include "reTgSend.h"
#endif // CONFIG_TELEGRAM_ENABLE

#define API_OPENMON_HOST "open-monitoring.online"
#define API_OPENMON_PORT 80
#define API_OPENMON_SEND_PATH "/get"
#define API_OPENMON_SEND_VALUES "cid=%d&key=%s&%s"
#define API_OPENMON_CHECK_INTERVAL 1000

typedef enum {
  OM_OK         = 0,
  OM_ERROR_API  = 1,
  OM_ERROR_HTTP = 2
} omSendStatus_t;

typedef struct omController_t {
  uint32_t id;
  const char* key;
  uint64_t last_send;
  uint8_t attempt;
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

bool omControllerInit(const uint32_t omId, const char * omKey)
{
  if (!_omControllers) {
    omControllersInit();
  };

  if (!_omControllers) {
    rlog_e(tagOM, "The controller list has not been initialized!");
    return false;
  };
    
  omControllerHandle_t ctrl = new omController_t;
  if (!ctrl) {
    rlog_e(tagOM, "Memory allocation error for data structure!");
    return false;
  };

  ctrl->id = omId;
  ctrl->key = omKey;
  ctrl->last_send = 0;
  ctrl->attempt = 0;
  ctrl->data = nullptr;
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
    if (item->data) free(item->data);
    delete item;
  };
  delete _omControllers;
}

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------ Adding data to the send queue ----------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool omSend(const uint32_t omId, char * omFields)
{
  if (_omQueue) {
    omQueueItem_t * item = new omQueueItem_t;
    if (item) {
      item->id = omId;
      item->data = omFields;
      if (xQueueSend(_omQueue, &item, CONFIG_OPENMON_QUEUE_WAIT / portTICK_RATE_MS) == pdPASS) {
        return true;
      };
    };
    rloga_e("Error adding message to queue [ %s ]!", omTaskName);
    ledSysStateSet(SYSLED_OTHER_PUB_ERROR, false);
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
  if (ctrl->data) {
    get_request = malloc_stringf(API_OPENMON_SEND_VALUES, ctrl->id, ctrl->key, ctrl->data);
  };

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
        if ((retCode == 200) || (retCode == 301)) {
          _result = OM_OK;
          rlog_i(tagOM, "Data sent # %d: %s", ctrl->id, get_request);
        } else {
          _result = OM_ERROR_API;
          rlog_e(tagOM, "Failed to send message, API error code: #%d!", retCode);
        };
      }
      else {
        _result = OM_ERROR_HTTP;
        rlog_e(tagOM, "Failed to complete request to open-monitoring.online, error code: 0x%x!", err);
      };
      esp_http_client_cleanup(client);
    }
    else {
      _result = OM_ERROR_HTTP;
      rlog_e(tagOM, "Failed to complete request to open-monitoring.online!");
    };
    ledSysOff(true);
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
  omSendStatus_t last_status = OM_ERROR_HTTP;
  bool api_enabled = true;
  
  while (true) {
    // Receiving new data
    if (xQueueReceive(_omQueue, &item, wait_queue) == pdPASS) {
      ctrl = omControllerFind(item->id);
      if (ctrl) {
        // Replacing controller data with new ones from the transporter
        if (item->data) {
          ctrl->attempt = 0;
          if (ctrl->data) free(ctrl->data);
          ctrl->data = item->data;
          item->data = nullptr;
        };
      } else {
        rlog_e(tagOM, "Controller # %d not found!", item->id);
      };
      // Free transporter
      if (item->data) free(item->data);
      delete item;
      item = nullptr;
    };

    // Check internet availability 
    if (wifiIsConnected() && wifiWaitConnection(0)) {
      if (!api_enabled) {
        api_enabled = true;
        ledSysStateClear(SYSLED_OTHER_PUB_ERROR, false);
        rlog_i(tagOM, "Internet access restored");
      };
      ctrl = nullptr;
      wait_queue = portMAX_DELAY;
      SLIST_FOREACH(ctrl, _omControllers, next) {
        if (ctrl->data) {
          // Sending data or finding the minimum delay
          if ((ctrl->last_send + CONFIG_OPENMON_MIN_INTERVAL) < millis()) {
            // Attempt to send data
            ctrl->attempt++;
            last_status = omSendEx(ctrl);
            switch (last_status) {
              // Data sent successfully
              case OM_OK:
                ctrl->last_send = millis();
                ctrl->attempt = 0;
                if (ctrl->data) {
                  free(ctrl->data);
                  ctrl->data = nullptr;
                };
                ledSysStateClear(SYSLED_OTHER_PUB_ERROR, false);
                break;

              // API rejected request
              case OM_ERROR_API:
                if (ctrl->attempt >= CONFIG_OPENMON_MAX_ATTEMPTS) {
                  ctrl->last_send = millis();
                  ctrl->attempt = 0;
                  if (ctrl->data) {
                    free(ctrl->data);
                    ctrl->data = nullptr;
                  };
                  rlog_e(tagOM, "Failed to send data to controller #%d!", ctrl->id);
                };
                ledSysStateSet(SYSLED_OTHER_PUB_ERROR, false);
                break;

              // Failed to send data due to transport error
              default:
                ledSysStateSet(SYSLED_OTHER_PUB_ERROR, false);
                break;
            };
          } else {
            // Find the minimum delay before the next sending to the controller
            TickType_t send_delay = ((ctrl->last_send + CONFIG_OPENMON_MIN_INTERVAL) - millis()) / portTICK_RATE_MS;
            if (send_delay < wait_queue) {
              wait_queue = send_delay;
            };
          };
        };
      };
    } else {
      // Internet not available
      wait_queue = API_OPENMON_CHECK_INTERVAL / portTICK_RATE_MS;
      if (api_enabled) {
        api_enabled = false;
        ledSysStateSet(SYSLED_OTHER_PUB_ERROR, false);
        rlog_w(tagOM, "No internet access, waiting...");
      };
    };
  };
  omTaskDelete();
}

// -----------------------------------------------------------------------------------------------------------------------
// -------------------------------------------------- Task routines ------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

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
      if (!omControllersInit()) {
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
        omControllersFree();
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
      omControllersFree();
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

