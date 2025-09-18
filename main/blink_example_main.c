#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_crt_bundle.h"
#include "driver/gpio.h"

#define WIFI_SSID      "KRISHNA"
#define WIFI_PASS      "12345678"
#define FIRMWARE_URL   "https://raw.githubusercontent.com/kbmkrishnamali1992-hub/ESP32_TRACKER_V1/main/firmware.bin"
#define BOOT_PIN       GPIO_NUM_13

static const char *TAG = "OTA_APP";

// Event group for OTA status
static EventGroupHandle_t ota_event_group;
#define OTA_SUCCESS_BIT BIT0
#define OTA_FAIL_BIT    BIT1

// Event group for WiFi connection
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

// Check if we should enter boot mode (GPIO 13 low)
static bool should_enter_boot_mode(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    
    // Read boot pin state
    int boot_pin_state = gpio_get_level(BOOT_PIN);
    ESP_LOGI(TAG, "Boot pin state: %d", boot_pin_state);
    
    return (boot_pin_state == 0); // Low = boot mode
}

// WiFi event handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base, 
                              int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// Wi-Fi initialization
static void wifi_init(void)
{
    ESP_LOGI(TAG, "Initializing WiFi");
    
    wifi_event_group = xEventGroupCreate();
    
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_WPA3_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(TAG, "Connecting to WiFi...");
    
    // Wait for WiFi connection with timeout
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(10000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected successfully");
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "WiFi connection failed");
    } else {
        ESP_LOGE(TAG, "WiFi connection timeout");
    }
}

// OTA Task
void ota_task(void *pvParameter)
{
    ESP_LOGI(TAG, "Free heap before OTA: %lu", (unsigned long)esp_get_free_heap_size());
    ESP_LOGI(TAG, "Starting OTA from: %s", FIRMWARE_URL);

    esp_http_client_config_t http_config = {
        .url = FIRMWARE_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_err_t ret = esp_https_ota(&ota_config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA successful!");
        xEventGroupSetBits(ota_event_group, OTA_SUCCESS_BIT);
    } else {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(ret));
        xEventGroupSetBits(ota_event_group, OTA_FAIL_BIT);
    }

    vTaskDelete(NULL);
}

// Main application task
void app_main_task(void *pvParameter)
{
    ESP_LOGI(TAG, "Running main application");
    
    // Your main application code here
    while (1) {
        ESP_LOGI(TAG, "Application running normally...");
        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
}

// App main
void app_main(void)
{
    ESP_LOGI(TAG, "Device starting up...");
    
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Check boot mode
    if (should_enter_boot_mode()) {
        ESP_LOGI(TAG, "Entering boot mode - checking for updates");
        
        // Initialize event group
        ota_event_group = xEventGroupCreate();
        
        // Initialize WiFi
        wifi_init();
        
        // Check if WiFi is connected
        if (xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT) {
            // Start OTA update task
            xTaskCreate(&ota_task, "ota_task", 16384, NULL, 5, NULL);
            
            // Wait for OTA to complete
            EventBits_t bits = xEventGroupWaitBits(ota_event_group, 
                                                 OTA_SUCCESS_BIT | OTA_FAIL_BIT,
                                                 pdFALSE, pdFALSE, portMAX_DELAY);
            
            if (bits & OTA_SUCCESS_BIT) {
                ESP_LOGI(TAG, "OTA successful, restarting in 3 seconds...");
                vTaskDelay(3000 / portTICK_PERIOD_MS);
                esp_restart();
            } else if (bits & OTA_FAIL_BIT) {
                ESP_LOGE(TAG, "OTA failed, continuing with current firmware");
            }
        } else {
            ESP_LOGE(TAG, "WiFi not connected, skipping OTA update");
        }
        
        vEventGroupDelete(ota_event_group);
        vEventGroupDelete(wifi_event_group);
    } else {
        ESP_LOGI(TAG, "Normal boot mode - running application");
    }

    // Start main application
    xTaskCreate(&app_main_task, "app_main_task", 4096, NULL, 5, NULL);
}