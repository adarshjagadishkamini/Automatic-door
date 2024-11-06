#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "mqtt_client.h"
#include "driver/gpio.h"

static const char *TAG = "DOOR";

// GPIO definitions for door control and object detection
#define DOOR_GPIO 0               // GPIO for door control (open/close)
#define PRESENCE_SENSOR_GPIO 1     // GPIO for object presence detection

// Semaphore to synchronize door control operations
SemaphoreHandle_t door_mutex;

// Function to initialize GPIOs
static void gpio_init(void)
{
    // Initialize GPIO for door control
    gpio_pad_select_gpio(DOOR_GPIO);
    gpio_set_direction(DOOR_GPIO, GPIO_MODE_OUTPUT);

    // Initialize GPIO for presence sensor
    gpio_pad_select_gpio(PRESENCE_SENSOR_GPIO);
    gpio_set_direction(PRESENCE_SENSOR_GPIO, GPIO_MODE_INPUT);
}

// Function to open the door (set GPIO to HIGH)
static void open_door(void)
{
    gpio_set_level(DOOR_GPIO, 1);  // Door open (HIGH)
    ESP_LOGI(TAG, "Door opened.");
}

// Function to close the door (set GPIO to LOW)
static void close_door(void)
{
    gpio_set_level(DOOR_GPIO, 0);  // Door closed (LOW)
    ESP_LOGI(TAG, "Door closed.");
}

// Function to check if an object is detected
static bool is_object_present(void)
{
    return gpio_get_level(PRESENCE_SENSOR_GPIO) == 1; // Object detected if GPIO is HIGH
}

// Task to handle door logic (open/close with auto-close feature)
static void door_task(void *pvParameters)
{
    while (1) {
        // Wait for door to open
        if (xSemaphoreTake(door_mutex, portMAX_DELAY) == pdTRUE) {
            open_door();  // Open the door

            // Wait for 5 seconds to simulate open time
            vTaskDelay(5000 / portTICK_PERIOD_MS);

            // Check for presence while trying to close
            while (is_object_present()) {
                ESP_LOGI(TAG, "Object detected, keeping door open.");
                vTaskDelay(5000 / portTICK_PERIOD_MS); // Wait for 5 more seconds
            }

            // After no object is detected, close the door
            close_door();
            xSemaphoreGive(door_mutex);  // Release the mutex
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

// Task to monitor presence sensor (if an object is detected)
static void presence_task(void *pvParameters)
{
    while (1) {
        if (is_object_present()) {
            ESP_LOGI(TAG, "Presence detected.");
            // Send MQTT message or publish door status
        }
        vTaskDelay(500 / portTICK_PERIOD_MS); // Check every 500ms
    }
}

// MQTT Event Handler
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32 "", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");

        // Subscribe to door command topic
        msg_id = esp_mqtt_client_subscribe(client, "/topic/door/command", 0);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        
        if (strstr(event->topic, "/topic/door/command") != NULL) {
            if (strstr(event->data, "Open") != NULL) {
                xSemaphoreTake(door_mutex, portMAX_DELAY);  // Protect the door action
                ESP_LOGI(TAG, "Open door command received.");
                open_door();
                xSemaphoreGive(door_mutex);  // Release mutex
            }
            if (strstr(event->data, "Close") != NULL) {
                xSemaphoreTake(door_mutex, portMAX_DELAY);  // Protect the door action
                ESP_LOGI(TAG, "Close door command received.");
                close_door();
                xSemaphoreGive(door_mutex);  // Release mutex
            }
        }
        break;

    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

// Function to initialize MQTT
static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_BROKER_URL,
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}

void app_main(void)
{
    ESP_LOGI(TAG, "[APP] Startup..");

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(example_connect());

    // Initialize GPIO
    gpio_init();

    // Create a binary semaphore for controlling door access
    door_mutex = xSemaphoreCreateMutex();
    if (door_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return;
    }

    // Start tasks for door control and presence monitoring
    xTaskCreate(door_task, "door_task", 2048, NULL, 5, NULL);  // Priority 5
    xTaskCreate(presence_task, "presence_task", 2048, NULL, 3, NULL);  // Priority 3

    // Start MQTT
    mqtt_app_start();
}
