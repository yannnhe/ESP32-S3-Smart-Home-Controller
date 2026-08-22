# ESP32-S3 智能家居控制器

基于 ESP32-S3 与 ESP-IDF 的智能家居控制器。项目正在将原有的 ESPHome 本科毕业设计迁移为原生 ESP-IDF 固件，以便获得更清晰的组件化结构、可控的联网行为、版本迭代能力，以及后续 Home Assistant/MQTT/OTA 集成能力。

> 当前阶段已完成 Wi-Fi STA 网络组件；传感器、执行器、Home Assistant 集成和 OTA 仍在迁移中。

## 当前功能

- ESP32-S3（16 MB Flash、Octal PSRAM）工程基础。
- 组件化 Wi-Fi STA 网络服务。
- 通过 `menuconfig` 配置固定 Wi-Fi SSID 与密码。
- WPA2/WPA3 支持、DHCP IPv4、RSSI/IP 查询。
- 断线后的指数退避持续重连：1、2、4、8、16、30 秒，随后保持 30 秒间隔。
- `NETWORK_EVENT_CONNECTED` / `NETWORK_EVENT_DISCONNECTED` 事件，供后续 MQTT、SNTP 和 OTA 服务订阅。
- 关闭 Wi-Fi 省电模式，优先保证 MQTT 与 OTA 的响应稳定性。

## 计划中的功能

- ADC、DHT11、火焰、MQ 系列、紫外线、光照、雨滴与水位传感器。
- WS2812B 灯带、RGB 模式指示灯、风扇、蜂鸣器与舵机控制。
- 本地按键、TM1637 时间显示和家庭模式状态机。
- Home Assistant MQTT Discovery、状态同步与本地自动化。
- HTTPS OTA、双 OTA 分区与新固件回滚。
- Wi-Fi SoftAP 配网与 Captive Portal。

## 工程结构

```text
.
├── components/
│   ├── network/          # 已实现：Wi-Fi STA、事件与重连状态机
│   ├── bsp/              # 板级支持包（迁移中）
│   ├── sensors/          # 传感器服务（计划）
│   ├── actuators/        # 执行器服务（计划）
│   ├── ha_mqtt/          # Home Assistant MQTT 接入（计划）
│   └── ota_service/      # HTTPS OTA 与回滚（计划）
├── main/                 # 系统初始化与组件启动顺序
├── partitions-16MiB.csv  # 16 MB Flash 分区表
├── dependencies.lock     # ESP-IDF Component Manager 锁定依赖
└── ESPHome到ESP-IDF迁移需求与实施计划.md
```

## 环境要求

- ESP-IDF 6.0.2 或兼容版本。
- ESP32-S3 开发板。
- Python、CMake、Ninja 和 Espressif Xtensa 工具链（由 ESP-IDF 安装器配置）。

## 快速开始

在 ESP-IDF 终端中执行：

```powershell
idf.py set-target esp32s3
idf.py reconfigure
idf.py menuconfig
```

在菜单中进入：

```text
Network Service Configuration
├── Wi-Fi SSID
└── Wi-Fi password
```

保存配置后构建、烧录并查看日志：

```powershell
idf.py build
idf.py -p COMx flash monitor
```

将 `COMx` 替换为开发板的串口号。首次联网成功时，日志会显示 IPv4 地址和 RSSI；Wi-Fi 断线后会自动按指数退避方式重连。

## 网络组件

`components/network` 将底层 `WIFI_EVENT` 和 `IP_EVENT` 转换为上层可使用的 `NETWORK_EVENT`：

```text
IP_EVENT_STA_GOT_IP          -> NETWORK_EVENT_CONNECTED
WIFI_EVENT_STA_DISCONNECTED  -> NETWORK_EVENT_DISCONNECTED
```

后续组件可通过 `network_is_connected()` 查询网络状态，或订阅 `NETWORK_EVENT` 来启动/停止 MQTT、SNTP、OTA 等网络相关服务。

## 安全与提交约定

- 不要提交 `sdkconfig`；其中可能含 Wi-Fi 凭据。
- 不要提交私钥、证书、`.env`、MQTT 密码或 OTA 下载令牌。
- 原始 `yaml/esp32-1.yaml` 包含 ESPHome API、OTA 和热点凭据，已由 `.gitignore` 排除。若需要公开配置参考，请先删除敏感值，再创建 `yaml/esp32-1.example.yaml`。
- 提交前建议执行：

```powershell
git status
git diff --check
```

## OTA（HTTP 固定源）

- OTA 使用 `components/ota_service` 和双 OTA 槽分区表；首次改用该分区表烧录时会重建应用分区布局，`vfs` 中的旧数据可能被覆盖。
- 固件下载地址只在 `menuconfig` 的 `OTA Service Configuration` 中配置，默认留空；服务只接受 host 为 `bin.bemfa.com` 的 `http://` 地址，MQTT 命令不能传入或覆盖 URL。不要将含令牌的 URL 提交到仓库或写入文档。
- Home Assistant MQTT Discovery 会创建 `OTA Status` 诊断 Sensor 和 `Start OTA` Button。Button 发布精确的 `START` 到 `smarthome/esp32-1/ota/set`；下载期间 Button 会变为不可用，下载或校验失败时自动恢复。
- 候选镜像必须为 ESP32-S3 镜像，且其内嵌的正整数版本必须高于当前运行版本；相同或更低版本会被拒绝。版本唯一来源是根目录 `CMakeLists.txt` 的 `set(PROJECT_VER "1")`：每次构建并上传巴法云固件前，手动递增它，例如 `1` 到 `2`。
- `sdkconfig.defaults` 已为新配置启用 Bootloader 的 app rollback。已有 `sdkconfig` 的工程还需在 `menuconfig -> Bootloader config -> Application Rollback` 中确认 `Enable app rollback support` 已启用。
- 新镜像重启后须在 120 秒内获得 IPv4、连接 Mosquitto MQTT，并完成一轮 HA Discovery/状态完整同步；否则会被标为无效并自动回滚。HTTP 下载不提供传输加密或来源身份验证，应只用于你信任的网络和固件托管源。

## 迁移说明

原 ESPHome 功能盘点、GPIO 对照、Home Assistant 实体规划、ESP-IDF 组件设计和实施阶段计划，见：[ESPHome到ESP-IDF迁移需求与实施计划.md](ESPHome到ESP-IDF迁移需求与实施计划.md)。

## 许可证

当前尚未指定许可证。在公开发布前，请根据是否允许他人使用、修改和商业分发选择并添加 `LICENSE` 文件，例如 MIT、Apache-2.0 或 GPL-3.0。
