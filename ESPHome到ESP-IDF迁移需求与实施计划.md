# 智能家居系统：ESPHome 到 ESP-IDF 迁移需求与实施计划

## 1. 范围与目标

目标硬件为 ESP32-S3 DevKitC-1，框架为 ESP-IDF。迁移以 MQTT + Home Assistant MQTT Discovery 替代 ESPHome Native API；不在 ESP-IDF 中解释 YAML。

当前迁移范围保留传感器、客厅灯带、本地灯按键、两路风扇、蜂鸣器与阳台窗舵机。Home Assistant 通过稳定 `unique_id`、统一 `esp32-1` Device、availability 和 retained 状态管理实体。

## 2. 保留硬件与 GPIO

| GPIO | 外设 | 主要行为 |
|---:|---|---|
| 1 | 客厅灯亮度旋钮 ADC | 200 ms，5 点中值滤波 |
| 2 | MQ2 ADC | 5 秒，需标定和输入保护 |
| 4 | GUVA-S12SD ADC | 5 秒，5 点中值滤波 |
| 5 | MQ135 ADC | 5 秒，需独立标定 |
| 6 | 光敏电阻 ADC | 5 秒，换算 lux |
| 7 | 雨滴 ADC | 5 秒，换算 0–100% |
| 8 | 厨房水位 ADC | 5 秒，换算 0–100% |
| 11 | WS2812B | 43 颗客厅灯带，限制总电流 |
| 12 | 卧室风扇 | 高电平开，上电安全关闭 |
| 13 / 15 / 16 | 三个 DHT11 | 客厅/阳台/卧室 |
| 14 | 客厅灯按键 | 去抖后切换灯带 |
| 18 | 厨房火焰传感器 | 低有效，外部上拉，50 ms 去抖 |
| 21 | 厨房风扇 | 高电平开，上电安全关闭 |
| 38 | 厨房蜂鸣器 | 低有效，上电保持关闭 |
| 48 | 阳台窗舵机 | 50 Hz PWM；最小/关闭/打开为 2.5%/7.5%/12.5% |

## 3. 已完成基线

- Wi-Fi STA、DHCP、自动重连、MQTT、LWT、availability 与 HA 状态订阅已完成。
- 已完成三个 DHT11 和 Wi-Fi RSSI 的 MQTT Discovery、缓存与 retained 状态。
- 已完成七路 ADC：GPIO1 亮度旋钮按 200 ms、5 点中值采样并以 1% 阈值发布；GPIO2、4、5、6、7、8 每 5 秒进行统一 5 点中值采样。ADC1 oneshot 使用曲线拟合校准，业务值沿用 YAML 的 MQ、UV、照度、雨滴和水位换算；不暴露原始电压，不直接控制灯带。
- 已完成厨房火焰 Binary Sensor：GPIO18 低有效、外部上拉，双边沿中断仅通知任务，50 ms 去抖后发布 retained `ON`/`OFF` 状态；HA Discovery 使用 `binary_sensor` 域与 `safety` device class。
- 已完成客厅 WS2812B MQTT Light：GPIO11、43 颗 GRB LED，支持 HA JSON 开关/亮度命令、GPIO14 高有效本地按键和 GPIO1 旋钮直接控灯。所有输入均先投递到灯带执行任务；实际刷新成功后才更新 retained 状态。
- `ha_discovery` 专用任务统一发布 Discovery/状态；MQTT 连接或 HA 上线时重发全部 Discovery 与当前状态。
- DHT 采样周期为 30 秒；RSSI 周期为 60 秒。

## 4. 传感器迁移要求

- 每个传感器组件负责初始化、独立采样任务、缓存与硬件错误处理。
- 组件注册 `ha_discovery` 状态组及 HA 实体；多值模块可共享一个 JSON 状态主题。
- 采样任务更新缓存后仅请求异步发布，不能直接调用 MQTT。
- ADC 使用 oneshot + 校准；一次业务采样可读取多次并滤波。MQ 类输入、除零、`NaN/Inf`、预热和标定必须处理。
- 读取失败须明确发布不可用状态，不能把失败结果当作 0。

## 5. 执行器与窗舵机要求

| 功能 | HA 建议实体 | 状态要求 |
|---|---|---|
| 客厅 WS2812B 灯带 | MQTT Light | 开关、亮度和 retained 状态 |
| 卧室/厨房风扇 | MQTT Switch | 发布实际逻辑输出状态 |
| 厨房蜂鸣器 | MQTT Switch | 发布实际逻辑输出状态 |
| 阳台窗舵机 | MQTT Switch，后续可改 Cover | 发布最后成功完成的逻辑位置 |

- 窗舵机使用约 1 秒平滑过渡，到位后停止 PWM。
- MQTT 回调只解析并投递命令；执行器任务负责实际动作、超时、互斥/合并和最终状态发布。
- 窗户当前没有限位反馈，状态仅表示最后成功命令；迁移前必须确认方向、脉宽、机械限位与上电安全位置。
- NVS 在动作成功后保存窗户逻辑位置；不在命令接收时提前报告成功。

## 6. MQTT 与 Home Assistant

基础主题为 `smarthome/esp32-1`：

| 用途 | 主题示例 | 策略 |
|---|---|---|
| 在线状态 | `smarthome/esp32-1/status` | retained，LWT=`offline` |
| 传感器状态 | `smarthome/esp32-1/<state_key>/state` | retained JSON 或文本 |
| 执行器命令 | `smarthome/esp32-1/<domain>/<id>/set` | 订阅，不 retained |
| 执行器状态 | `smarthome/esp32-1/<domain>/<id>/state` | retained，动作完成后发布 |
| Discovery | `homeassistant/<component>/<unique_id>/config` | retained |

Discovery、状态与命令均需稳定命名、正确 device class/unit/state class 和 `esp32-1` 设备标识。关键消息使用 QoS 1。

## 7. 安全与硬件风险

- MQ 模块可能输出超过 ESP32-S3 ADC 允许电压，必须确认分压、共地和供电。
- 43 颗 WS2812B 的电源应独立 5 V、共地、串联数据电阻，并限制软件亮度。
- 风扇、蜂鸣器和舵机不能由 GPIO 直接带载，需确认驱动、电源、续流和共地。
- 上电先将灯带、风扇、蜂鸣器置于安全关闭状态，再恢复经确认安全的窗户逻辑位置。

## 8. 分阶段计划与验收

| 阶段 | 目标 | 验收 |
|---|---|---|
| P0 | 核对开发板、接线、电平、电源与 GPIO 风险 | 风险项有明确结论 |
| P1 | 验证既有网络、MQTT、DHT、ADC、火焰、灯带、RSSI | HA 自动发现正确，重连后状态完整恢复，ADC 量程和换算经实机核对 |
| P2 | 强化 ADC 标定与传感器长稳测试 | 每路独立稳定，异常输入不崩溃 |
| P3 | 迁移风扇、蜂鸣器与命令路由复用 | 命令、硬件状态和 HA 状态闭环一致 |
| P4 | 迁移阳台窗舵机与 NVS | 动作安全、到位后停 PWM、重启恢复策略正确 |
| P5 | OTA、持久化强化与长稳测试 | 升级/断网/掉电/异常输入可恢复 |

## 9. 约束

- 不提交或输出任何凭据；YAML 仅作参考且可能含敏感内容。
- 不修改 `managed_components/`；依赖通过 `idf_component.yml` 管理。
- 组件按网络、MQTT、公共发布、传感器、执行器分层；事件回调不执行阻塞硬件操作。
- 每次开发前检查 `git status`，保留与当前任务无关的工作区改动。
