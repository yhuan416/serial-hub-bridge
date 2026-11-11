#include "bridge_button.h"

#include "sdkconfig.h"

#if CONFIG_BRIDGE_BUTTON_ENABLE

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

#include "bridge_prov.h"

#define TAG "bridge_button"
#define BTN_GPIO ((gpio_num_t)CONFIG_BRIDGE_BUTTON_GPIO)

static TimerHandle_t s_timer;

static void long_press_cb(TimerHandle_t t)
{
    (void)t;
    ESP_LOGW(TAG, "long press: factory reset WiFi provisioning");
    bridge_prov_factory_reset();
}

static void IRAM_ATTR btn_isr(void *arg)
{
    (void)arg;
    BaseType_t woken = pdFALSE;
    if (gpio_get_level(BTN_GPIO) == 0) {
        xTimerStartFromISR(s_timer, &woken);
    } else {
        xTimerStopFromISR(s_timer, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

esp_err_t bridge_button_init(void)
{
    s_timer = xTimerCreate("btn_reset", pdMS_TO_TICKS(CONFIG_BRIDGE_BUTTON_LONG_PRESS_MS),
                           pdFALSE, NULL, long_press_cb);
    if (!s_timer) {
        return ESP_ERR_NO_MEM;
    }

    const gpio_config_t cfg = {
        .intr_type = GPIO_INTR_ANYEDGE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = 1ULL << CONFIG_BRIDGE_BUTTON_GPIO,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service: %s", esp_err_to_name(err));
        return err;
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(BTN_GPIO, btn_isr, NULL));

    ESP_LOGI(TAG, "GPIO%d long-press %d ms -> factory reset",
             CONFIG_BRIDGE_BUTTON_GPIO, CONFIG_BRIDGE_BUTTON_LONG_PRESS_MS);
    return ESP_OK;
}

#else

esp_err_t bridge_button_init(void)
{
    return ESP_OK;
}

#endif