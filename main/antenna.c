#include "antenna.h"
#include "settings.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "antenna";

void antenna_apply(void)
{
    static bool s_on;
    int sel = g_set.antenna_ext ? 1 : 0;
    if (!s_on) {
        /* Levels first, then outputs, so neither pin glitches; the switch gets its selection before its power. */
        gpio_set_level(ANTENNA_SEL_GPIO, sel);
        gpio_set_level(ANTENNA_EN_GPIO, 0);
        gpio_config_t c = { .pin_bit_mask = BIT64(ANTENNA_SEL_GPIO) | BIT64(ANTENNA_EN_GPIO), .mode = GPIO_MODE_OUTPUT };
        ESP_ERROR_CHECK(gpio_config(&c));
        s_on = true;
    } else gpio_set_level(ANTENNA_SEL_GPIO, sel);
    ESP_LOGI(TAG, "%s antenna", sel ? "external" : "built-in");
}
