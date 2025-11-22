#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

// 定义一个日志标签
static const char *TAG = "hello_world";

void app_main(void)
{
    // 使用 ESP-IDF 的日志宏打印信息
    ESP_LOGI(TAG, "Hello world!");

    // 循环打印，并使用 vTaskDelay 来让出 CPU
    for (int i = 10; i >= 0; i--) {
        ESP_LOGI(TAG, "Restarting in %d seconds...", i);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    ESP_LOGI(TAG, "Restarting now.");
    fflush(stdout);
    esp_restart(); // 重启 ESP32
}