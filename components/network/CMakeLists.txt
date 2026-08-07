# 注册 network 为标准 ESP-IDF 组件。
# 组件源码与对外头文件分开放置，方便其他组件只通过 network.h 使用网络服务。
idf_component_register(
    # network.c 包含 STA 初始化、事件处理、重连状态机以及状态查询接口。
    SRCS
        "network.c"

    # include 目录中的 network.h 会暴露给依赖 network 的其他组件。
    INCLUDE_DIRS
        "include"

    # network.h 对外公开了 ESP_EVENT_DECLARE_BASE，因此 esp_event 必须作为
    # 公共依赖向使用者传递头文件搜索路径。
    REQUIRES
        esp_event

    # 以下组件只在 network.c 内部使用，不应成为 network 公共接口的一部分。
    PRIV_REQUIRES
        esp_netif
        esp_timer
        esp_wifi
)
