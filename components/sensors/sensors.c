#include "sensors.h"

#include "adc.h"
#include "binary.h"
#include "dht11.h"

esp_err_t sensors_init(void)
{
    esp_err_t err = dht11_init();
    if (err != ESP_OK) return err;

    err = adc_init();
    if (err != ESP_OK) return err;

    return binary_init();
}
