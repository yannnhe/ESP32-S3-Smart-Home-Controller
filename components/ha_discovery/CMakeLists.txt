# Home Assistant MQTT Sensor Discovery 与状态发布公共层。
idf_component_register(
    SRCS
        "ha_discovery.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_event
        freertos
        ha_mqtt
)
