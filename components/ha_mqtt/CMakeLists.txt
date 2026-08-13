# 注册 Home Assistant MQTT 通信组件。
idf_component_register(
    SRCS
        "ha_mqtt.c"
    INCLUDE_DIRS
        "include"

    # 公共头文件声明了 ESP-IDF 自定义事件基，因此需要向使用者传递 esp_event。
    REQUIRES
        esp_event

    # MQTT 客户端、MAC 地址读取以及 network 事件仅在组件内部使用。
    PRIV_REQUIRES
        esp_hw_support
        mqtt
        network
)
