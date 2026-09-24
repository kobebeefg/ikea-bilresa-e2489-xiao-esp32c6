#include "light_driver.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// XIAO ESP32-C6: user LED is active LOW on GPIO15.
void light_driver_set_power(bool power)
{
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_15, power ? 0 : 1));
}

void light_driver_init(bool power)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << GPIO_NUM_15),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    // Brief boot indication confirms that the user LED driver is working.
    for (int i = 0; i < 3; i++) {
        light_driver_set_power(true);
        vTaskDelay(pdMS_TO_TICKS(120));
        light_driver_set_power(false);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    light_driver_set_power(power);
}
