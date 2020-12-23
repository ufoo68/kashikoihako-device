/*
 * AWS IoT EduKit - Blinky Hello World v1.0.0
 * Copyright 2010-2015 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * Additions Copyright 2016 Espressif Systems (Shanghai) PTE LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */
/**
 * @file main.c
 * @brief simple MQTT publish and subscribe for use with AWS IoT EduKit reference hardware.
 *
 * This example takes the parameters from the build configuration and establishes a connection to AWS IoT Core over MQTT.
 *
 * Some configuration is required. Visit https://edukit.workshop.aws
 *
 */

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
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "core2forAWS.h"
#include "lvgl/lvgl.h"
#include "ft6336u.h"
#include "axp192.h"
#include "cryptoauthlib.h"
#include "i2c_device.h"
#include "atecc608a.h"

#include "aws_iot_config.h"
#include "aws_iot_log.h"
#include "aws_iot_version.h"
#include "aws_iot_mqtt_client_interface.h"

#include "blink.h"
#include "display.h"
#include "servo.h"

static const char *TAG = "Blinky";

/* The examples use simple WiFi configuration that you can set via
   'make menuconfig'.

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/
#define EXAMPLE_WIFI_SSID CONFIG_WIFI_SSID
#define EXAMPLE_WIFI_PASS CONFIG_WIFI_PASSWORD

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event */
const int CONNECTED_BIT = BIT0;
const int DISCONNECTED_BIT = BIT1;

// in disp_spi.c
extern SemaphoreHandle_t xGuiSemaphore;
extern SemaphoreHandle_t spi_mutex;
// Used for reading/writing from SD_Card
extern void spi_poll();

TaskHandle_t xBlink;

/* CA Root certificate, device ("Thing") certificate and device
 * ("Thing") key.

   Example can be configured one of two ways:

   "Embedded Certs" are loaded from files in "certs/" and embedded into the app binary.

   "Filesystem Certs" are loaded from the filesystem (SD card, etc.)

   See example README for more details.
*/
#if defined(CONFIG_AWS_IOT_USE_HARDWARE_SECURE_ELEMENT)

extern const uint8_t aws_root_ca_pem_start[] asm("_binary_aws_root_ca_pem_start");
extern const uint8_t aws_root_ca_pem_end[] asm("_binary_aws_root_ca_pem_end");

#elif defined(CONFIG_EXAMPLE_EMBEDDED_CERTS)

extern const uint8_t aws_root_ca_pem_start[] asm("_binary_aws_root_ca_pem_start");
extern const uint8_t aws_root_ca_pem_end[] asm("_binary_aws_root_ca_pem_end");
extern const uint8_t certificate_pem_crt_start[] asm("_binary_certificate_pem_crt_start");
extern const uint8_t certificate_pem_crt_end[] asm("_binary_certificate_pem_crt_end");
extern const uint8_t private_pem_key_start[] asm("_binary_private_pem_key_start");
extern const uint8_t private_pem_key_end[] asm("_binary_private_pem_key_end");

#elif defined(CONFIG_EXAMPLE_FILESYSTEM_CERTS)

static const char * DEVICE_CERTIFICATE_PATH = CONFIG_EXAMPLE_CERTIFICATE_PATH;
static const char * DEVICE_PRIVATE_KEY_PATH = CONFIG_EXAMPLE_PRIVATE_KEY_PATH;
static const char * ROOT_CA_PATH = CONFIG_EXAMPLE_ROOT_CA_PATH;

#else
#error "Invalid method for loading certs"
#endif

/**
 * @brief Default MQTT HOST URL is pulled from the aws_iot_config.h
 */
char HostAddress[255] = AWS_IOT_MQTT_HOST;

/**
 * @brief Default MQTT port is pulled from the aws_iot_config.h
 */
uint32_t port = AWS_IOT_MQTT_PORT;

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        display_wifi_label_update(true);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        /* This is a workaround as ESP32 WiFi libs don't currently
           auto-reassociate. */
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        xEventGroupSetBits(wifi_event_group, DISCONNECTED_BIT);
        display_wifi_label_update(false);
        break;
    default:
        break;
    }
    return ESP_OK;
}

void iot_subscribe_callback_handler(AWS_IoT_Client *pClient, char *topicName, uint16_t topicNameLen,
                                    IoT_Publish_Message_Params *params, void *pData) {
    ESP_LOGI(TAG, "Subscribe callback");
    ESP_LOGI(TAG, "%.*s\t%.*s", topicNameLen, topicName, (int) params->payloadLen, (char *)params->payload);
    if (strstr(topicName, "/blink") != NULL) {
        // Get state of the FreeRTOS task, "blinkTask", using it's task handle.
        // Suspend or resume the task depending on the returned task state
        eTaskState blinkState = eTaskGetState(xBlink);
        if (blinkState == eSuspended){
            vTaskResume(xBlink);
        } else{
            vTaskSuspend(xBlink);
        }
    }
}

void disconnect_callback_handler(AWS_IoT_Client *pClient, void *data) {
    ESP_LOGW(TAG, "MQTT Disconnect");
    display_textarea_add("Disconnected from AWS IoT Core...", NULL, NULL);
    IoT_Error_t rc = FAILURE;

    if(NULL == pClient) {
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

void aws_iot_task(void *param) {
    char cPayload[100];

    int32_t i = 0;

    IoT_Error_t rc = FAILURE;

    AWS_IoT_Client client;
    IoT_Client_Init_Params mqttInitParams = iotClientInitParamsDefault;
    IoT_Client_Connect_Params connectParams = iotClientConnectParamsDefault;

    IoT_Publish_Message_Params paramsQOS0;
    IoT_Publish_Message_Params paramsQOS1;

    ESP_LOGI(TAG, "AWS IoT SDK Version %d.%d.%d-%s", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, VERSION_TAG);

    mqttInitParams.enableAutoReconnect = false; // We enable this later below
    mqttInitParams.pHostURL = HostAddress;
    mqttInitParams.port = port;

#if defined(CONFIG_AWS_IOT_USE_HARDWARE_SECURE_ELEMENT)
    
    mqttInitParams.pRootCALocation = (const char *)aws_root_ca_pem_start;
    mqttInitParams.pDeviceCertLocation = "#";
    mqttInitParams.pDevicePrivateKeyLocation = "#0";
    
    char * clientId = malloc(ATCA_SERIAL_NUM_SIZE * 2 + 1);
    ATCA_STATUS ret = Atecc608a_GetSerialString(clientId);
    if (ret != ATCA_SUCCESS){
        ESP_LOGE(TAG, "Failed to get device serial from secure element. Error: %i", ret);
        abort();
    }
    #define CLIENT_ID clientId
    #define CLIENT_ID_LEN strlen(clientId)
    char subTopic[CLIENT_ID_LEN + 2];
    char pubTopic[CLIENT_ID_LEN + 1];
    sprintf(subTopic, "%s/#", CLIENT_ID);
    sprintf(pubTopic, "%s/", CLIENT_ID);
    #define SUBSCRIBE_TOPIC subTopic
    #define PUBLISH_TOPIC pubTopic

#elif defined(CONFIG_EXAMPLE_EMBEDDED_CERTS)
    mqttInitParams.pRootCALocation = (const char *)aws_root_ca_pem_start;
    mqttInitParams.pDeviceCertLocation = (const char *)certificate_pem_crt_start;
    mqttInitParams.pDevicePrivateKeyLocation = (const char *)private_pem_key_start;
    #define SUBSCRIBE_TOPIC "topicfilter/"
    #define PUBLISH_TOPIC "topic/"
    #define CLIENT_ID CONFIG_AWS_EXAMPLE_CLIENT_ID

#elif defined(CONFIG_EXAMPLE_FILESYSTEM_CERTS)
    mqttInitParams.pRootCALocation = ROOT_CA_PATH;
    mqttInitParams.pDeviceCertLocation = DEVICE_CERTIFICATE_PATH;
    mqttInitParams.pDevicePrivateKeyLocation = DEVICE_PRIVATE_KEY_PATH;
    #define SUBSCRIBE_TOPIC "topicfilter/"
    #define PUBLISH_TOPIC "topic/"
    #define CLIENT_ID CONFIG_AWS_EXAMPLE_CLIENT_ID

#endif

    mqttInitParams.mqttCommandTimeout_ms = 20000;
    mqttInitParams.tlsHandshakeTimeout_ms = 5000;
    mqttInitParams.isSSLHostnameVerify = true;
    mqttInitParams.disconnectHandler = disconnect_callback_handler;
    mqttInitParams.disconnectHandlerData = NULL;

#ifdef CONFIG_EXAMPLE_SDCARD_CERTS
    ESP_LOGI(TAG, "Mounting SD card...");
    spi_poll();
    xSemaphoreTake(spi_mutex)
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 3,
    };
    sdmmc_card_t* card;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card VFAT filesystem. Error: %s", esp_err_to_name(ret));
        abort();
    }
#endif

    rc = aws_iot_mqtt_init(&client, &mqttInitParams);
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

    connectParams.pClientID = CLIENT_ID;
    connectParams.clientIDLen = CLIENT_ID_LEN;
    connectParams.isWillMsgPresent = false;
    display_textarea_add("Connecting to AWS IoT Core...\n", NULL, NULL);
    ESP_LOGI(TAG, "Connecting to AWS IoT Core...");
    do {
        rc = aws_iot_mqtt_connect(&client, &connectParams);
        if(SUCCESS != rc) {
            ESP_LOGE(TAG, "Error(%d) connecting to %s:%d", rc, mqttInitParams.pHostURL, mqttInitParams.port);
            vTaskDelay(1000 / portTICK_RATE_MS);
        }
    } while(SUCCESS != rc);
    display_textarea_add("Successfully connected!\n", NULL, NULL);
    ESP_LOGI(TAG, "Successfully connected to AWS IoT Core!");
    /*
     * Enable Auto Reconnect functionality. Minimum and Maximum time of Exponential backoff are set in aws_iot_config.h
     *  #AWS_IOT_MQTT_MIN_RECONNECT_WAIT_INTERVAL
     *  #AWS_IOT_MQTT_MAX_RECONNECT_WAIT_INTERVAL
     */
    rc = aws_iot_mqtt_autoreconnect_set_status(&client, true);
    if(SUCCESS != rc) {
        display_textarea_add("Unable to set Auto Reconnect to true\n", NULL, NULL);
        ESP_LOGE(TAG, "Unable to set Auto Reconnect to true - %d", rc);
        abort();
    }

    const int SUBSCRIBE_TOPIC_LEN = strlen(SUBSCRIBE_TOPIC);    
    ESP_LOGI(TAG, "Subscribing...");
    rc = aws_iot_mqtt_subscribe(&client, SUBSCRIBE_TOPIC, SUBSCRIBE_TOPIC_LEN, QOS0, iot_subscribe_callback_handler, NULL);
    if(SUCCESS != rc) {
        display_textarea_add("Error subscribing\n", NULL, NULL);
        ESP_LOGE(TAG, "Error subscribing : %d ", rc);
        abort();
    } else{
        display_textarea_add("Subscribed to topic: %s\n", SUBSCRIBE_TOPIC, (size_t *) SUBSCRIBE_TOPIC_LEN) ;
        ESP_LOGI(TAG, "Subscribed to topic: %s", SUBSCRIBE_TOPIC);
    }

    sprintf(cPayload, "%s : %d ", "hello from SDK", i);

    paramsQOS0.qos = QOS0;
    paramsQOS0.payload = (void *) cPayload;
    paramsQOS0.isRetained = 0;

    paramsQOS1.qos = QOS1;
    paramsQOS1.payload = (void *) cPayload;
    paramsQOS1.isRetained = 0;
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "**************************************");
    ESP_LOGI(TAG, "Client Id - %s", CLIENT_ID);
    ESP_LOGI(TAG, "**************************************\n\n");

    const int PUBLISH_TOPIC_LEN = strlen(PUBLISH_TOPIC);
    
    while((NETWORK_ATTEMPTING_RECONNECT == rc || NETWORK_RECONNECTED == rc || SUCCESS == rc)) {

        //Max time the yield function will wait for read messages
        rc = aws_iot_mqtt_yield(&client, 100);
        if(NETWORK_ATTEMPTING_RECONNECT == rc) {
            // If the client is attempting to reconnect we will skip the rest of the loop.
            continue;
        }

        ESP_LOGD(TAG, "Stack remaining for task '%s' is %d bytes", pcTaskGetTaskName(NULL), uxTaskGetStackHighWaterMark(NULL));
        vTaskDelay(3000 / portTICK_RATE_MS);
        
        // Publish and ignore if "ack" was received or  from AWS IoT Core
        sprintf(cPayload, "%s : %d ", "Hello from AWS IoT EduKit (QOS0)", i++);
        paramsQOS0.payloadLen = strlen(cPayload);
        rc = aws_iot_mqtt_publish(&client, PUBLISH_TOPIC, PUBLISH_TOPIC_LEN, &paramsQOS0);
        if (rc != SUCCESS){
            ESP_LOGE(TAG, "Publish QOS0 error %i", rc);
            rc = SUCCESS;
        }

        // Publish and check if "ack" was sent from AWS IoT Core
        sprintf(cPayload, "%s : %d ", "Hello from AWS IoT EduKit (QOS1)", i++);
        paramsQOS1.payloadLen = strlen(cPayload);
        rc = aws_iot_mqtt_publish(&client, PUBLISH_TOPIC, PUBLISH_TOPIC_LEN, &paramsQOS1);
        if (rc == MQTT_REQUEST_TIMEOUT_ERROR) {
            ESP_LOGW(TAG, "QOS1 publish ack not received.");
            rc = SUCCESS;
        }
    }

    ESP_LOGE(TAG, "An error occurred in the main loop.");
    abort();
}

static void initialise_wifi(void)
{
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_WIFI_SSID,
            .password = EXAMPLE_WIFI_PASS,
        },
    };
    ESP_LOGI(TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );
}


void app_main()
{
    // Initialize NVS.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK( err );
    
    spi_mutex = xSemaphoreCreateMutex();

    Core2ForAWS_Init();
    FT6336U_Init();
    Core2ForAWS_LCD_Init();
    Core2ForAWS_Button_Init();
    Core2ForAWS_Sk6812_Init();
    Core2ForAWS_LCD_SetBrightness(80);
    
    display_init();
    
    // initialise_wifi();

    #if defined (CONFIG_AWS_IOT_USE_HARDWARE_SECURE_ELEMENT)
        
        ATCA_STATUS ret = Atecc608a_Init();
        if (ret != ATCA_SUCCESS){
            ESP_LOGE(TAG, "ATECC608A secure element initialization error");
            abort();
        }
    
    #endif

        // xTaskCreatePinnedToCore(&aws_iot_task, "aws_iot_task", 9216, NULL, 5, NULL, 1);
        xTaskCreatePinnedToCore(&blink_task, "blink_task", 4096*1, NULL, 2, &xBlink, 1);
        xTaskCreatePinnedToCore(&servo_task, "servo_task", 4096*1, NULL, 1, NULL, 1);
}
