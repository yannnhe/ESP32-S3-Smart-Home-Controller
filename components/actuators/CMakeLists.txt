# 执行器业务组件：本地输入、MQTT 命令和实际硬件输出的串行闭环。
idf_component_register(
    SRCS
        "actuators.c"
        "living_room_light.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        driver
        freertos
        ha_discovery
        ha_mqtt
        cjson
        led_strip
        sensors
)
