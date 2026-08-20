#include "actuators.h"

#include "balcony_window.h"
#include "living_room_light.h"
#include "output_switches.h"

esp_err_t actuators_init(void)
{
    esp_err_t err = living_room_light_init();
    if (err != ESP_OK) return err;

    err = output_switches_init();
    if (err != ESP_OK) return err;

    return balcony_window_init();
}
