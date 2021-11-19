# reOpenMon for ESP32

**EN**: Sending sensor data to http://open-monitoring.online/ with a specified interval and sending queue. For ESP32 only, since it was released as a FreeRTOS task and on ESP32-specific functions. Controller field values (data) are passed to the queue as a string (char*), which is automatically deleted after sending. That is, to send, you must place a line with data on the heap, and then send it to the library queue.

> Controller parameters must be set in "project_config.h" (see below for description)

**RU**: Отправка данных сенсоров на http://open-monitoring.online/ с заданным интервалом и очередью отправки. Только для ESP32, так как релизовано в виде задачи FreeRTOS и на специфических для ESP32 функциях. Значения полей контроллера (данные) передаются в очередь в виде строки (char*), которая автоматически удаляется после отправки. То есть для отправки Вы должны разместить в куче строку с данными, а затем отправить ее в очередь библиотеки.

> Параметры контроллера должны быть заданы в "project_config.h" (описание см. ниже)

## Using / Использование:

<i>// Task management: create, suspend, resume and delete</i><br/>
<i>// Управление задачей: создание, приостановка, восстановление и удаление</i><br/>

bool omTaskCreate();
bool omTaskSuspend();
bool omTaskResume();
bool omTaskDelete();

<i>// Controller initialization</i><br/>
<i>// Создание контроллера.</i><br/>
<i>// @param om_id - Controller ID / Идентификатор контроллера</i><br/>
<i>// @param om_key - Controller token / Токен контроллера</i><br/>

om_ctrl_t * omInitController(const uint32_t om_id, const char * om_key);

<i>// Removing a controller and free resources</i><br/>
<i>// Удаление контроллера и освобождение ресурсов</i><br/>

void omFreeController(om_ctrl_t * omController);

<i>// Sending data to the specified controller. The fields string will be removed after submission.</i><br/>
<i>// If little time has passed since the last data sent to the controller, the data will be queued.</i><br/>
<i>// If there is already data in the queue for this controller, it will be overwritten with new data.</i><br/>

<i>// Отправка данных в заданный контроллер. Строка fields будет удалена после отправки. </i><br/>
<i>// Если с момента последней отправки данных в контроллер прошло мало времени, то данные будут поставлены в очередь.</i><br/>
<i>// Если в очереди на данный контроллер уже есть данные, то они будут перезаписаны новыми данными.</i><br/>
<i>// @param omController - Pointer to controller from omInitController / Указатель на контроллер из omInitController </i><br/>
<i>// @param fields - Data in the format p1=... / Данные в формате p1=...</i><br/>

bool omSend(om_ctrl_t * omController, char * fields);

## Dependencies / Зависимости:
- esp_http_client.h (ESP-IDF)
- https://github.com/kotyara12/rLog
- https://github.com/kotyara12/rStrings
- https://github.com/kotyara12/reEsp32
- https://github.com/kotyara12/reEvents
- https://github.com/kotyara12/reWiFi

## Parameters / Параметры:

<i>// Enable sending data to open-monitoring.online</i><br/>
<i>// Включить отправку данных на open-monitoring.online</i><br/>
#define CONFIG_OPENMON_ENABLE 1<br/>
#if CONFIG_OPENMON_ENABLE<br/>
<i>// Frequency of sending data in seconds</i><br/>
<i>// Периодичность отправки данных в секундах</i><br/>
#define CONFIG_OPENMON_SEND_INTERVAL 180<br/>
<i>// Minimum server access interval in milliseconds (for each controller separately)</i><br/>
<i>// Минимальный интервал обращения к серверу в миллисекундах (для каждого контроллера отдельно)</i><br/>
#define CONFIG_OPENMON_MIN_INTERVAL 60000<br/>
<i>// Stack size for the task of sending data to open-monitoring.online</i><br/>
<i>// Размер стека для задачи отправки данных на open-monitoring.online</i><br/>
#define CONFIG_OPENMON_STACK_SIZE 2048<br/>
<i>// Queue size for the task of sending data to open-monitoring.online</i><br/>
<i>// Размер очереди для задачи отправки данных на open-monitoring.online</i><br/>
#define CONFIG_OPENMON_QUEUE_SIZE 8<br/>
<i>// Timeout for writing to the queue for sending data on open-monitoring.online</i><br/>
<i>// Время ожидания записи в очередь отправки данных на open-monitoring.online</i><br/>
#define CONFIG_OPENMON_QUEUE_WAIT 100<br/>
<i>// The priority of the task of sending data to open-monitoring.online</i><br/>
<i>// Приоритет задачи отправки данных на open-monitoring.online</i><br/>
#define CONFIG_OPENMON_PRIORITY 5<br/>
<i>// Processor core for the task of sending data to open-monitoring.online</i><br/>
<i>// Ядро процессора для задачи отправки данных на open-monitoring.online</i><br/>
#define CONFIG_OPENMON_CORE 1<br/>
<i>// The number of attempts to send data to open-monitoring.online</i><br/>
<i>// Количество попыток отправки данных на open-monitoring.online</i><br/>
#define CONFIG_OPENMON_MAX_ATTEMPTS 3<br/>
<i>// Controller ids and tokens for open-monitoring.online</i><br/>
<i>// Идентификаторы контроллеров и токены для open-monitoring.online</i><br/>
#define CONFIG_OPENMON_CTR01_ID 0001<br/>
#define CONFIG_OPENMON_CTR01_TOKEN "aaaaaa"<br/>
#define CONFIG_OPENMON_CTR02_ID 0002<br/>
#define CONFIG_OPENMON_CTR02_TOKEN "bbbbbb"<br/>
#define CONFIG_OPENMON_CTR03_ID 0003<br/>
#define CONFIG_OPENMON_CTR03_TOKEN "cccccc"<br/>
#endif // CONFIG_OPENMON_ENABLE<br/>
