#include "actuators.h"

#include "living_room_light.h"

esp_err_t actuators_init(void)
{
    return living_room_light_init();
}
