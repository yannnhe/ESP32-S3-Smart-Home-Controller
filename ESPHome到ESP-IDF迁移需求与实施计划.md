# 智能家居系统：ESPHome 到 ESP-IDF 迁移需求与实施计划

## 1. 文档范围

本文依据 `yaml/esp32-1.yaml` 整理，目标硬件为 ESP32-S3 DevKitC-1。迁移目标不是在 ESP-IDF 中解释 YAML，而是用 ESP-IDF 原生驱动、任务和通信组件重新实现同等功能。

推荐的 Home Assistant 接入方式是 **MQTT + Home Assistant MQTT Discovery**。ESPHome Native API 包含实体协议、Protobuf、加密和连接管理，脱离 ESPHome 后自行兼容的开发及维护成本很高；MQTT 是 ESP-IDF 与 Home Assistant 都有成熟支持、且便于调试的接口。

## 2. 现有系统总体能力

系统覆盖客厅、厨房、阳台和卧室，具备以下能力：

1. 采集 7 路 ADC 输入、3 个 DHT11 温湿度传感器、1 个数字火焰传感器以及 Wi-Fi RSSI。
2. 控制 43 颗 WS2812B 客厅灯带、2 个风扇、1 个蜂鸣器、2 个舵机、1 个 RGB 模式指示灯。
3. 使用 1 个客厅灯按键和 4 个家庭模式按键进行本地控制。
4. 通过 Home Assistant 控制和显示设备，并同步 `home/away/sleep/emergency` 四种模式。
5. 使用 TM1637 四位数码管显示时间并闪烁冒号。
6. 保存门和窗的逻辑位置，重启后恢复。
7. 提供 Wi-Fi、后备热点、日志、加密的 ESPHome API 和 ESPHome OTA。
8. 上电时先关闭高负载设备，再恢复门窗位置，最后根据家庭模式改变指示灯。

## 3. 完整硬件与 GPIO 清单

| GPIO | 方向/外设 | 硬件或用途 | 原配置行为 | ESP-IDF 实现要点 |
|---:|---|---|---|---|
| 1 | ADC1 | 客厅灯亮度旋钮 | 200 ms；5 点中值滤波 | ADC oneshot + 校准；软件中值滤波 |
| 2 | ADC1 | MQ2 可燃气体 | 5 s；电压转 ppm | ADC 校准；预热、零点及除零保护 |
| 4 | ADC1 | GUVA-S12SD 紫外线 | 5 s；每次平均 10 个样本 | 多次采样平均；分段转 UV 指数 |
| 5 | ADC1 | MQ135 空气质量 | 5 s；电压转 ppm | ADC 校准；预热和标定 |
| 6 | ADC1 | 光敏电阻 | 5 s；电阻转 lux | 检查分压拓扑并保留限幅 |
| 7 | ADC1 | 雨滴传感器 | 5 s；电压转 0–100% | ADC 校准和上下限标定 |
| 8 | ADC1 | 厨房水位 | 5 s；电压转 0–100% | ADC 校准和上下限标定 |
| 11 | RMT TX | 43 颗 WS2812B | GRB、总亮度控制 | `led_strip`/RMT；限制总电流 |
| 12 | GPIO OUT | 卧室风扇开关 | 高电平开 | 上电立即输出安全电平 |
| 13 | 单总线 GPIO | 客厅 DHT11 | 温度、湿度，5 s | 已引入 `esp-idf-lib/dht` |
| 14 | GPIO IN | 客厅灯按键 | 按下切换灯带 | 明确外部上下拉；增加去抖 |
| 15 | 单总线 GPIO | 阳台 DHT11 | 温度、湿度，5 s | 三个 DHT 建议错峰读取 |
| 16 | 单总线 GPIO | 卧室 DHT11 | 温度、湿度，5 s | 失败重试但不阻塞主控制 |
| 17 | GPIO | TM1637 DIO | 数据线 | 自建或引入 TM1637 驱动 |
| 18 | GPIO | TM1637 CLK | 时钟线 | 500 ms 刷新 |
| 19 | GPIO IN | 厨房火焰传感器 | 低电平表示触发 | 与原生 USB D- 复用，见风险项 |
| 21 | GPIO OUT | 厨房风扇开关 | 高电平开 | 上电立即输出安全电平 |
| 35 | GPIO IN PU | 在家模式按键 | 低有效，20 ms 去抖 | 需先确认模组 Flash/PSRAM 占用 |
| 36 | GPIO IN PU | 离家模式按键 | 同上 | 同上 |
| 37 | GPIO IN PU | 紧急模式按键 | 同上 | 同上 |
| 38 | GPIO IN PU | 睡眠模式按键 | 同上 | 检查开发板可用性 |
| 39 | LEDC | 模式指示灯 R | 1 kHz PWM | 3 路 LEDC 同步控制 |
| 40 | LEDC | 模式指示灯 G | 1 kHz PWM | 同上 |
| 41 | LEDC | 模式指示灯 B | 1 kHz PWM | 同上 |
| 42 | GPIO OUT | 厨房蜂鸣器 | 低有效 | 上电保持高电平关闭 |
| 47 | LEDC | 门锁/门舵机 | 50 Hz PWM | 1 s 渐变，随后停 PWM |
| 48 | LEDC | 阳台窗舵机 | 50 Hz PWM | 2.5%/7.5%/12.5% 占空比 |

### 上板前必须核对

- 当前 `sdkconfig` 启用了 16 MB Flash 和 Octal PSRAM。部分 ESP32-S3 模组的 GPIO35–37 会被 Octal Flash/PSRAM 占用，不能按普通 GPIO 使用。必须依据开发板完整型号/模组丝印和原理图确认；若被占用，四个模式按键应整体换脚。
- GPIO19 是 USB D-。若使用板载原生 USB/USB Serial-JTAG 下载、日志或调试，火焰传感器不能继续占用 GPIO19。
- MQ2、MQ135 等模块常由 5 V 供电，模拟输出可能高于 ESP32-S3 ADC 允许电压。必须实测并配置分压或缓冲，不能只靠软件限幅。
- WS2812B 43 灯全白时理论电流可能达到约 2.6 A。应使用独立 5 V 电源、共地、数据串联电阻，并设置软件最大亮度/功率限制。
- 风扇、蜂鸣器和舵机不得由 GPIO 直接带载；需确认 MOSFET/三极管、续流二极管、电源和共地设计。

## 4. 传感器功能与算法需求

| 编号 | 对外实体 | 原始输入 | 周期 | 换算/处理 | 对外单位 |
|---|---|---|---:|---|---|
| S01 | Knob ADC voltage | GPIO1 ADC | 200 ms | 5 点滑动中值 | V |
| S02 | LED Intensity Index - Living Room | S01 | 200 ms | `(V-0.100)/(3.157-0.100)×100`，限幅 0–100 | % |
| S03 | Flammable Gas - Kitchen | GPIO2 ADC | 5 s | MQ 电阻模型和幂函数换算 | ppm |
| S04 | UV index - Balcony | GPIO4 ADC | 5 s | 10 次平均；0.05–0.976 V 分段映射到 0–10 | 无 |
| S05 | Air Quality - Balcony | GPIO5 ADC | 5 s | 当前与 MQ2 使用相同的电阻和幂函数 | ppm |
| S06 | Light intensity - Balcony | GPIO6 ADC | 5 s | 计算 LDR 电阻，再用经验公式换算；0–999 限幅 | lux |
| S07 | Rain Index - Balcony | GPIO7 ADC | 5 s | 干 3.157 V、湿 1.500 V 线性映射 | % |
| S08/S09 | 客厅温度/湿度 | DHT11 GPIO13 | 5 s | DHT11 读取、有效性检查 | °C / % |
| S10/S11 | 阳台温度/湿度 | DHT11 GPIO15 | 5 s | 同上 | °C / % |
| S12/S13 | 卧室温度/湿度 | DHT11 GPIO16 | 5 s | 同上 | °C / % |
| S14 | Water Level - Kitchen | GPIO8 ADC | 5 s | 0.100–3.157 V 线性映射 | % |
| S15 | ESP32-1 wifi signal | Wi-Fi | 30 s | `esp_wifi_sta_get_ap_info()` 获取 RSSI | dBm |
| B01 | Fire Sensor - Kitchen | GPIO19 | 事件驱动 | 低有效，建议 20–50 ms 去抖 | 开/关 |

### 旋钮控制的完整行为

1. 首次采样仅记录位置，不改变灯带，防止重启后灯光突然跳变。
2. 旋钮相对上次位置变化小于 5% 时视为噪声。
3. 新亮度相对上次已下发亮度变化小于 3% 时不重复写灯。
4. 亮度不大于 1% 时关灯，否则开灯并设置亮度。
5. 本地旋钮、按键与远程 MQTT 命令必须经过同一个灯光状态机，避免状态互相覆盖。

### 算法迁移时需要补充的保护

- ADC 电压必须来自 ESP-IDF ADC 校准结果，而不是直接按原始码值假定 3.3 V。
- MQ 公式在电压接近 0 V 时会除零；在电压超出供电范围时会产生负值或无穷值。需对输入、`RS`、`NaN/Inf` 做保护。
- MQ2/MQ135 应有预热状态；预热完成前发布 `unavailable` 或诊断值，不发布看似有效的 ppm。
- 当前 MQ2 与 MQ135 使用完全相同的 `Vcc=5.0`、`RL=0.5 kΩ`、`R0=6.64 kΩ` 和曲线。正式项目应分别标定，并在 NVS 中保存可更新参数。
- DHT、ADC 读取失败时保留最后有效值并标记质量/可用性，不能把失败值当作 0。

## 5. 执行器、交互与自动化需求

| 编号 | 功能 | Home Assistant 建议实体 | 本地控制 | 状态反馈 |
|---|---|---|---|---|
| A01 | 客厅 WS2812B 灯带 | MQTT Light（开关、亮度，可选 RGB） | GPIO14 按键、GPIO1 旋钮 | retained 状态主题 |
| A02 | 卧室风扇 | MQTT Switch | 无 | 实际输出逻辑状态 |
| A03 | 厨房风扇 | MQTT Switch | 无 | 实际输出逻辑状态 |
| A04 | 厨房蜂鸣器 | MQTT Switch | 可由报警规则控制 | 实际输出逻辑状态 |
| A05 | 门锁/门舵机 | 第一阶段保持 MQTT Switch；后续可改 Lock | 无 | NVS 中的逻辑位置 |
| A06 | 阳台窗舵机 | 第一阶段保持 MQTT Switch；后续可改 Cover | 无 | NVS 中的逻辑位置 |
| A07 | 家庭模式 RGB 指示灯 | 内部功能，不必对外暴露 | 四个模式键 | 当前模式 |
| A08 | TM1637 时间显示 | 内部功能 | 无 | 本地时钟 |

### 舵机行为

- 门：打开写入 `level=-1`；关闭写入 `level=0.2`。ESP-IDF 实现前应从 ESPHome 默认舵机最小/空闲/最大占空比确认门的实际脉宽，不应只复制抽象 `level`。
- 窗：打开为 12.5% 占空比，关闭为 7.5%，最小端为 2.5%。
- 两个舵机均用 50 Hz PWM、约 1 s 平滑过渡；到位后停止 PWM，减少发热和抖动。
- 命令完成后再写 NVS、更新 MQTT 状态。若移动失败或复位，不得提前报告成功。
- 当前系统没有门窗限位开关，因此“位置”只是最后命令，不是真实物理反馈。安全要求较高时应增加限位/位置传感器。

### 家庭模式

| 模式 | 指示灯 |
|---|---|
| `home` | 绿色，50% |
| `away` | 蓝色，50% |
| `sleep` | 紫色（R=60%、B=80%），30% |
| `emergency` | 红色，100%，500 ms 渐变/500 ms 周期脉冲 |
| 未知/未同步 | 白色，15% |
| 启动中 | 白色，10% |

四个模式按键经 20 ms 按下/释放去抖后发出模式请求。推荐让 ESP32 暴露一个 MQTT Select（四个选项）并让 Home Assistant 以它作为模式实体；若必须保留 `input_select.home_modes`，则增加两条 HA 自动化：

1. ESP32 发布 `homeassistant/.../mode/request` 后，HA 调用 `input_select.select_option`。
2. `input_select.home_modes` 改变后，HA 将最终值发布到 ESP32 订阅的 retained 模式状态主题。

设备只根据“最终状态主题”改变指示灯，避免请求失败时本地与 HA 显示不一致。

## 6. 启动、状态持久化与故障行为

### 与现配置等价的启动流程

1. 初始化日志、NVS、事件循环和看门狗。
2. 在其他任务启动前设置安全输出：灯带、两风扇、蜂鸣器关闭，舵机 PWM 停止。
3. 模式指示灯显示 10% 白色。
4. 等待外设电源稳定 1 s。
5. 从 NVS 读取门窗逻辑状态并执行恢复动作。
6. 启动 Wi-Fi、时间同步和 MQTT。
7. 收到 retained 家庭模式后立即更新指示灯；超时则保持未知模式白色，不用固定等待 5 s。
8. 发布设备 online、全量实体状态及诊断信息。

### 建议的安全修正

- 门锁不建议无条件按 NVS 的旧值动作。推荐定义“上电默认锁闭/等待授权”的失效安全策略，并单独确认。
- NVS 只保存门、窗及标定参数；频繁变化的灯光亮度不必每次写入，避免 Flash 磨损。
- 使用 NVS schema/version 字段，升级固件时可迁移或重置旧数据。
- 检测 brownout、watchdog 和复位原因并上报诊断实体。
- MQTT 断线时本地按键、传感器和安全控制应继续工作；状态在重连后补发。

## 7. Home Assistant 通信需求

### 推荐 MQTT 主题模型

建议基础主题为 `smarthome/esp32-1`：

| 用途 | 示例主题 | 策略 |
|---|---|---|
| 在线状态 | `smarthome/esp32-1/status` | retained；LWT=`offline` |
| 传感器状态 | `smarthome/esp32-1/sensor/<id>/state` | 周期发布；可按变化阈值抑制 |
| 执行器命令 | `smarthome/esp32-1/<domain>/<id>/set` | 订阅 |
| 执行器状态 | `smarthome/esp32-1/<domain>/<id>/state` | retained；动作完成后发布 |
| 模式请求 | `smarthome/esp32-1/mode/request` | 非 retained |
| 最终模式 | `smarthome/esp32-1/mode/state` | retained |
| 诊断 | `smarthome/esp32-1/diagnostic/<id>` | 周期/事件发布 |
| Discovery | `homeassistant/<component>/<unique_id>/config` | retained |

### Discovery 实体最低集合

- 15 个 sensor。
- 1 个 binary_sensor（火焰）。
- 1 个 light（客厅灯带）。
- 5 个 switch（蜂鸣器、两个风扇、门、窗）；若门窗改为 `lock`/`cover`，相应减少 switch。
- 1 个 select（家庭模式，推荐）。
- 可选 4 个 button（若希望 HA 中分别显示四个模式动作）。
- 诊断实体：IP、MAC、固件版本、运行时间、复位原因、堆内存、MQTT/Wi-Fi 重连次数。

所有实体需要稳定 `unique_id`、一致的 device 标识、正确的 device class/state class、单位和 availability。MQTT 回调只解析并投递命令到队列，不在 MQTT 任务中执行 1 s 舵机动作或其他阻塞操作。

### 网络、安全和配置

- Wi-Fi 与 MQTT 地址、端口、用户名/密码不能硬编码进 Git；使用 Kconfig 默认值仅供开发，正式凭据通过 NVS provisioning 写入。
- 推荐 MQTT over TLS，并校验服务器证书；至少应使用独立的最小权限 MQTT 账号。
- 实现指数退避重连，并区分 Wi-Fi 断开、DNS、TLS、认证和 broker 拒绝。
- 原 YAML 中存在明文 ESPHome API/OTA 密钥和后备热点密码。迁移后应废弃/轮换这些凭据，文档和日志不得再次输出。
- 若要求保留“Wi-Fi 失败后开启 AP + 配网页面”，可使用 ESP-IDF Wi-Fi Provisioning Manager 的 SoftAP 方案；若必须像 ESPHome 一样浏览器自动弹出，还需补充 DNS 劫持和 HTTP captive portal。

## 8. 时间、显示与 OTA

### 时间和 TM1637

- ESPHome 原先从 Home Assistant 取时；ESP-IDF 推荐用 SNTP。
- 配置时区（中国标准时间可使用 POSIX TZ `CST-8`），时间有效前显示 `----`，不能显示 1970 年时间。
- 每 500 ms 更新 TM1637；交替显示 `HH.MM` 和 `HHMM` 达到冒号闪烁效果。
- 网络掉线后继续依靠系统时钟运行，并周期重同步。

### OTA

当前分区表只有一个 `factory` 应用分区，没有 `ota_0`、`ota_1` 和 `otadata`，无法实现标准的安全双分区 OTA。迁移需：

1. 根据最终固件大小设计 16 MB 分区表，至少包含 NVS、`otadata`、`ota_0`、`ota_1` 和数据分区。
2. 使用 `esp_https_ota`，强制 HTTPS 证书校验。
3. 启用新固件首启自检、`esp_ota_mark_app_valid_cancel_rollback()` 和失败回滚。
4. 通过 MQTT 命令或 HA Button 触发 OTA，但 URL、版本和授权必须校验。
5. 保留串口救援刷机路径。

## 9. 推荐 ESP-IDF 软件结构

```text
main/
  app_main.c                 # 只负责初始化顺序和服务启动
components/
  app_config/                # Kconfig、版本、运行配置
  bsp/                       # GPIO 定义、安全初值、板型自检
  storage/                   # NVS schema、门窗状态、标定值
  network/                   # Wi-Fi、provisioning、SNTP
  ha_mqtt/                   # MQTT、Discovery、主题、LWT
  sensors/                   # ADC 管理、换算、DHT、火焰、RSSI
  actuators/                 # 灯带、风扇、蜂鸣器、舵机、RGB
  local_ui/                  # 按键、模式状态机、TM1637
  automation/                # 启动流程、本地联动、命令队列
  ota_service/               # HTTPS OTA、自检和回滚
```

建议使用一个中心 `app_state`（带互斥锁或消息队列）保存有效状态。驱动层不直接发布 MQTT；传感器和执行器产生事件，由 `ha_mqtt` 统一发布。这样本地按键、远程命令和启动恢复都走相同状态机。

### 建议任务/定时器

| 任务 | 周期/触发 | 说明 |
|---|---|---|
| ADC 采样任务 | 旋钮 200 ms；其他通道 5 s | 串行管理 ADC1，统一校准 |
| DHT 任务 | 每个 5 s，彼此错峰 | 避免同时阻塞和电源扰动 |
| GPIO 按键/火焰 | 中断 + 软件定时器/队列 | ISR 只记录事件 |
| 执行器任务 | 命令队列 | 负责舵机渐变、灯光和输出 |
| 显示任务 | 500 ms | 只读取已同步的系统时间 |
| MQTT | 事件驱动 | 命令解析、状态与 Discovery |
| 诊断任务 | 30–60 s | RSSI、内存、运行时间 |

## 10. 当前 ESP-IDF 工程基线与需整改项

已确认：

- 目标芯片是 ESP32-S3，Flash 配置 16 MB，Octal PSRAM 已启用。
- `app_main()` 当前为空。
- `main/idf_component.yml` 已加入 `esp-idf-lib/dht ^1.2.0`。
- 当前分区表不支持双分区 OTA。
- `components/bsp/CMakeList.txt` 文件名少了 `s`，ESP-IDF 不会把它当作组件的 `CMakeLists.txt`。
- 根 `CMakeLists.txt` 设置了 `EXTRA_COMPONENT_DIRS components/Middlewares`，但当前未发现该目录；普通项目内组件可直接放在标准 `components/` 下。
- BSP 文件当前没有源文件和 include 目录，并启用了全局倾向的 `-ffast-math`/`-O3`。传感器公式涉及 `NaN/Inf` 检查，`-ffast-math` 可能破坏这些语义，不建议对整个组件盲目启用。
- 原 YAML 的部分注释和单位已出现乱码（例如引号和 kΩ）。迁移时应统一将源码保存为 UTF-8，并按公式语义而不是乱码文本确认单位。
- 工程目录当前不是可识别的 Git 工作树；正式迁移前建议建立版本基线。

## 11. 分阶段实施计划

| 阶段 | 主要任务 | 交付物 | 完成/验收条件 |
|---|---|---|---|
| P0 需求冻结 | 核对开发板完整型号、原理图、电源、GPIO、传感器型号及逻辑电平；确定 MQTT broker 和模式实体方案 | 引脚确认表、通信约定、风险签字项 | GPIO35–37、GPIO19 和所有 5 V ADC 风险关闭 |
| P1 工程骨架 | 修正组件目录；建立 BSP/config/storage；定义错误码、日志和状态模型 | 可编译的组件化空框架 | 启动无错误，所有危险输出保持关闭 |
| P2 底层驱动 | 实现 ADC 校准、DHT、GPIO、LEDC、RMT 灯带、TM1637；做单元/台架测试 | 各驱动测试程序或测试模式 | 每个外设可独立稳定运行，连续测试无复位 |
| P3 业务算法 | 移植换算公式、滤波、旋钮阈值、家庭模式、舵机渐变和启动状态机 | 本地离线可运行固件 | 不接 HA 时按键、灯、显示和传感器正常 |
| P4 网络和时间 | Wi-Fi、provisioning、SNTP、断线重连 | 可配网、可校时固件 | 路由器重启后自动恢复；时间显示正确 |
| P5 HA 集成 | MQTT TLS、Discovery、实体命令/状态、LWT、模式同步 | HA 自动发现全部实体 | 命令状态闭环、重连补发、无重复实体 |
| P6 持久化与恢复 | NVS schema、门窗策略、配置迁移、Flash 写频控制 | 掉电恢复功能 | 随机断电测试后状态符合安全策略 |
| P7 OTA | 重做分区表；HTTPS OTA；首启验证与回滚 | 可安全升级的双分区固件 | 正常升级成功；故意损坏版本自动回滚 |
| P8 系统验证 | 压力、断网、掉电、传感器异常、MQTT 洪泛、长稳测试 | 测试报告和已知问题表 | 目标场景全部通过，至少 24–72 h 稳定运行 |

## 12. 详细验收用例

### 启动与安全

- 冷启动后两风扇、蜂鸣器和灯带不得出现瞬时误动作。
- 网络不可用时仍能完成本地初始化且不会循环复位。
- 模式未同步时为白色指示；收到四种模式时颜色和紧急脉冲正确。
- 门窗恢复符合最终确定的失效安全策略。

### 传感器

- 每路 ADC 用万用表/标准电压源抽测至少 3 点，记录校准误差。
- 旋钮静止时不抖动下发；跨越 5% 阈值时灯光改变；首次读数不改变灯。
- 每个 DHT 断线、短路或返回校验错误时不会把 0 当有效值发布。
- MQ 预热期、0 V、满量程和断线均不会产生 `NaN/Inf` 或任务崩溃。
- 火焰输入触发和恢复均正确上报。

### 执行器

- MQTT、本地按钮和旋钮交替操作时，HA 最终状态与硬件一致。
- 舵机 1 s 平滑到位并停止 PWM；连续命令有互斥/合并策略。
- 网络断线不改变当前风扇/灯状态，除非安全策略明确要求。
- WS2812 高亮运行时 ESP32 不 brownout，电源和器件温升合格。

### 通信与恢复

- MQTT broker 重启、Wi-Fi 路由器重启、错误密码、DNS 失败均有明确日志且可恢复。
- 设备异常断电后 LWT 显示 unavailable；重连后所有实体状态重新发布。
- Discovery 重发不产生重复设备或实体。
- 未授权 MQTT 客户端不能控制设备。

### OTA

- 正常升级保持 NVS 配置。
- 下载中断仍可从旧分区启动。
- 新固件启动自检失败能自动回滚。
- 固件来源、TLS 证书和版本规则均校验。

## 13. 开发优先级

### 必须完成（MVP）

- 上电安全、全部传感器、全部执行器、本地按键、家庭模式、TM1637。
- Wi-Fi + MQTT Discovery + 状态/命令闭环。
- NVS、断线重连、基本错误处理和看门狗。
- 修正 GPIO/电压风险。

### 正式交付前必须完成

- TLS、安全 provisioning、凭据轮换。
- 双分区 HTTPS OTA 和回滚。
- 标定参数、长稳测试、异常输入测试、文档和接线图。

### 可选增强

- 门改为 HA Lock、窗改为 Cover，并增加真实限位反馈。
- 火焰/MQ2 超阈值的本地离线报警与风扇联动。
- Web 配置页、远程日志、指标统计。
- 传感器阈值、采样周期、模式颜色通过 MQTT/NVS 动态配置。

## 14. 开工前需要最终确认的决策

1. ESP32-S3 开发板和模组的完整型号，以及 GPIO35–37 是否真正可用。
2. 是否仍需使用原生 USB；若需要，火焰传感器从 GPIO19 迁出。
3. Home Assistant 是否已有 MQTT broker；推荐以 MQTT 替代 ESPHome Native API。
4. 家庭模式是改为 MQTT Select，还是保留 `input_select.home_modes` 并使用 HA 自动化桥接。
5. 门锁上电策略：恢复旧状态、默认锁闭，还是等待 HA 同步后再动作。
6. 是否需要完全复刻 fallback captive portal，还是接受 ESP-IDF Provisioning Manager。
7. MQ2/MQ135 的模块型号、分压电路、实际 `RL/R0` 和目标检测气体。
8. 门窗舵机实际脉宽、方向、机械限位和安全位置。
