# main 负责系统级初始化和各业务组件的启动编排。
idf_component_register(
    SRCS
        "main.c"
    INCLUDE_DIRS
        "."

    # main.c 直接使用默认事件循环、esp_netif、network 与 NVS API。
    REQUIRES
        esp_event
        esp_netif
        network
        nvs_flash
)
