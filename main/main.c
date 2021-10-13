#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_log.h"

#include "aws_iot_config.h"
#include "aws_iot_log.h"
#include "aws_iot_version.h"
#include "aws_iot_mqtt_client_interface.h"

#include "core2forAWS.h"

#include "wifi.h"
#include "blink.h"
#include "ui.h"

#include "tflite_main.h"
//#include "mpu.h"
#include "sd.h"
#include "sntp_sync.h"
//#include "data_batch.h"
#include "mic.h"
//#include "ota_task.h"

//#include "iotex_task.h"


/* The time between each MQTT message publish in milliseconds */
#define PUBLISH_INTERVAL_MS 3000
#define OTA_DO_NOT_USE_CUSTOM_CONFIG


/* The time prefix used by the logger. */
static const char *TAG = "MAIN";

/* The FreeRTOS task handler for the blink task that can be used to control the task later */
TaskHandle_t xBlink;
extern QueueHandle_t xQueueBatchData;

/* CA Root certificate */
extern const uint8_t aws_root_ca_pem_start[] asm("_binary_aws_root_ca_pem_start");
extern const uint8_t aws_root_ca_pem_end[] asm("_binary_aws_root_ca_pem_end");

/* Default MQTT HOST URL is pulled from the aws_iot_config.h */
char HostAddress[255] = AWS_IOT_MQTT_HOST;

/* Default MQTT port is pulled from the aws_iot_config.h */
uint32_t port = AWS_IOT_MQTT_PORT;

AWS_IoT_Client mqtt_client;

/* FreeRTOS event group to signal when we are connected & ready to make a request */
EventGroupHandle_t mqtt_event_group;

SemaphoreHandle_t xMQTTSemaphore;

/* The event group allows multiple bits for each event */
#define CONNECTED_BIT BIT0
#define DISCONNECTED_BIT BIT1



void disconnect_callback_handler(AWS_IoT_Client *pClient, void *data) {
    ESP_LOGW(TAG, "MQTT Disconnect");
    IoT_Error_t rc = FAILURE;
    if(pClient == NULL) {
        return;
    }
    if(aws_iot_is_autoreconnect_enabled(pClient)) {
        ESP_LOGI(TAG, "Auto Reconnect is enabled, Reconnecting attempt will start now");
    } else {
        ESP_LOGW(TAG, "Auto Reconnect not enabled. Starting manual reconnect...");
        rc = aws_iot_mqtt_attempt_reconnect(pClient);
        if(NETWORK_RECONNECTED == rc) {
            ESP_LOGW(TAG, "Manual Reconnect Successful");
        } else {
            ESP_LOGW(TAG, "Manual Reconnect Failed - %d", rc);
        }
    }
}


void iot_subscribe_callback_handler2(AWS_IoT_Client *pClient, char *topicName, uint16_t topicNameLen,
                                    IoT_Publish_Message_Params *params, void *pData) 
{
    ESP_LOGI(TAG, "Subscribe callback");
    ESP_LOGI(TAG, "%.*s\t%.*s", topicNameLen, topicName, (int) params->payloadLen, (char *)params->payload);
}

void aws_iot_task(void *param) {
    
    xMQTTSemaphore = xSemaphoreCreateMutex();

    mqtt_event_group = xEventGroupCreate();
    xEventGroupClearBits(mqtt_event_group, CONNECTED_BIT);
    xEventGroupSetBits(mqtt_event_group, DISCONNECTED_BIT);
    
    IoT_Error_t rc = FAILURE;
    
    IoT_Client_Init_Params mqttInitParams = iotClientInitParamsDefault;
    IoT_Client_Connect_Params connectParams = iotClientConnectParamsDefault;

    ESP_LOGI(TAG, "AWS IoT SDK Version %d.%d.%d-%s", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, VERSION_TAG);

    mqttInitParams.enableAutoReconnect = false; // We enable this later below
    mqttInitParams.pHostURL = HostAddress;
    mqttInitParams.port = port;    
    mqttInitParams.pRootCALocation = (const char *)aws_root_ca_pem_start;
    mqttInitParams.pDeviceCertLocation = "#";
    mqttInitParams.pDevicePrivateKeyLocation = "#0";
    
#define CLIENT_ID_LEN (ATCA_SERIAL_NUM_SIZE * 2)
#define SUBSCRIBE_TOPIC_LEN (CLIENT_ID_LEN + 3)
#define BASE_PUBLISH_TOPIC_LEN (CLIENT_ID_LEN + 2)

    char *client_id = malloc(CLIENT_ID_LEN + 1);
    ATCA_STATUS ret = Atecc608_GetSerialString(client_id);
    if (ret != ATCA_SUCCESS)
    {
        printf("Failed to get device serial from secure element. Error: %i", ret);
        abort();
    }

    char subscribe_topic[SUBSCRIBE_TOPIC_LEN];
    snprintf(subscribe_topic, SUBSCRIBE_TOPIC_LEN, "%s/#", client_id);
    
    mqttInitParams.mqttCommandTimeout_ms = 20000;
    mqttInitParams.tlsHandshakeTimeout_ms = 5000;
    mqttInitParams.isSSLHostnameVerify = true;
    mqttInitParams.disconnectHandler = disconnect_callback_handler;
    mqttInitParams.disconnectHandlerData = NULL;

    rc = aws_iot_mqtt_init(&mqtt_client, &mqttInitParams);
    if(SUCCESS != rc) {
        ESP_LOGE(TAG, "aws_iot_mqtt_init returned error : %d ", rc);
        abort();
    }

    /* Wait for WiFI to show as connected */
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,
                        false, true, portMAX_DELAY);    

    connectParams.keepAliveIntervalInSec = 10;
    connectParams.isCleanSession = true;
    connectParams.MQTTVersion = MQTT_3_1_1;
    connectParams.pClientID = client_id;
    connectParams.clientIDLen = CLIENT_ID_LEN;
    connectParams.isWillMsgPresent = false;

    ui_textarea_add("Connecting to AWS IoT Core...\n", NULL, 0);
    ESP_LOGI(TAG, "Connecting to AWS IoT Core at %s:%d", mqttInitParams.pHostURL, mqttInitParams.port);
    do {
        rc = aws_iot_mqtt_connect(&mqtt_client, &connectParams);
        if(SUCCESS != rc) {
            ESP_LOGE(TAG, "Error(%d) connecting to %s:%d", rc, mqttInitParams.pHostURL, mqttInitParams.port);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    } while(SUCCESS != rc);

    ESP_LOGI(TAG, "Successfully connected to AWS IoT Core!");

    rc = aws_iot_mqtt_autoreconnect_set_status(&mqtt_client, true);
    if(SUCCESS != rc) {
        ui_textarea_add("Unable to set Auto Reconnect to true\n", NULL, 0);
        ESP_LOGE(TAG, "Unable to set Auto Reconnect to true - %d", rc);
        abort();
    }

    xEventGroupClearBits(mqtt_event_group, DISCONNECTED_BIT);
    xEventGroupSetBits(mqtt_event_group, CONNECTED_BIT);
/*
    ESP_LOGI(TAG, "Subscribing to '%s'", subscribe_topic);
    rc = aws_iot_mqtt_subscribe(&mqtt_client, subscribe_topic, strlen(subscribe_topic), QOS0, iot_subscribe_callback_handler2, NULL);
    if(SUCCESS != rc) {
        ESP_LOGE(TAG, "Error subscribing : %d ", rc);
    } else{
        ESP_LOGI(TAG, "Subscribed to topic '%s'", subscribe_topic);
    }  
*/

    while((NETWORK_ATTEMPTING_RECONNECT == rc || NETWORK_RECONNECTED == rc || SUCCESS == rc)) {

        if(xSemaphoreTake(xMQTTSemaphore,pdMS_TO_TICKS(100)) == pdTRUE ) {

            rc = aws_iot_mqtt_yield(&mqtt_client, 100);
            printf("rc: %i\r\n", rc);
            ESP_LOGI(TAG, "Stack remaining for task '%s' is %d bytes", pcTaskGetTaskName(NULL), uxTaskGetStackHighWaterMark(NULL));
            xSemaphoreGive(xMQTTSemaphore);
        } else {

            ESP_LOGI(TAG, "Could not take semaphore");
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
   
    ESP_LOGE(TAG, "An error occurred in the main loop.");
    abort();
}


void app_main()
{
    Core2ForAWS_Init();
    
    Core2ForAWS_Display_SetBrightness(80);
    
    ui_init();

    initialise_wifi();
    
    //xTaskCreatePinnedToCore(&sd_task, "sd_task", 4096 * 2, NULL, 1, NULL, 1);

    //xTaskCreatePinnedToCore(&data_batch_task, "data_batch_task", 4096 * 2, NULL, 1, NULL, 1);
    
    //xTaskCreatePinnedToCore(&mic_task, "mic_task", 4096 * 2, NULL, 5, NULL, 1);
    
    //xTaskCreatePinnedToCore(&mpu_task, "mpu_task", 4096 * 2, NULL, 5, NULL, 1);
    
    //xTaskCreatePinnedToCore(&sntp_task, "sntp_task", 4096 * 2, NULL, 5, NULL, 1);

    // xTaskCreatePinnedToCore(&ota_task, "ota_task", 4096 * 2, NULL, 5, NULL, 1);

    xTaskCreatePinnedToCore(&app_main_tflite, "tflite_main_task", 1024 * 32, NULL, 8, NULL, 1);

    // xTaskCreatePinnedToCore(&aws_iot_task, "aws_iot_task", 4096 * 5, NULL, 1, NULL, 1);

    // xTaskCreatePinnedToCore(&blink_task, "blink_task", 4096 * 1, NULL, 2, &xBlink, 1);

    // xTaskCreatePinnedToCore(&iotex_task, "iotex_task", 4096 * 4, NULL, 5, NULL, 1);
}