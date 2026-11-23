| Supported Targets | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C6 | ESP32-H2 | ESP32-P4 | ESP32-S2 | ESP32-S3 |
| ----------------- | ----- | -------- | -------- | -------- | -------- | -------- | -------- | -------- |

# WiTime ESP32-S3 Wi-Fi 时钟

本项目使用 ESP-IDF、FreeRTOS、DS3231、WS2812 16×16 点阵和 BluFi/Wi-Fi APSTA 配网。

## 接线

- DS3231 SDA -> GPIO8，SCL -> GPIO9，VCC -> 3V3，GND 共地。
- WS2812 DIN -> GPIO10（建议串联约 330Ω），使用独立 5V 供电并与 ESP32 共地。
- 默认点阵映射为逐行蛇形/弓型，支持镜像和旋转校正。

## 启动逻辑

上电后循环播放启动动画，最少显示 8 秒；RTC、Wi-Fi 和网络时间任务同时运行。设备优先直连上次成功的 Wi-Fi，获得 IP 后显示 1.5 秒联网成功页面，并立即启动 SNTP；定位和天气在独立任务中并行刷新。网络状态尚未确定时最多等待到开机 15 秒，随后只要 RTC 或 SNTP 已提供有效时间就进入主页面；连续 3 次连接失败时显示 1.5 秒失败页面并继续后台重试。SNTP 成功后更新系统时间并回写 DS3231，天气使用 Open-Meteo，默认每 10 分钟刷新。

## 设备命名（蓝牙 / WiFi 主机名 / 热点名统一）

设备的**蓝牙广播名**、**WiFi 主机名（STA hostname，路由器 DHCP 客户端列表里显示的名字）**、**SoftAP 热点名**使用**同一个字符串**，格式为：

```
ESPARK_<WiFi STA MAC 后六位，十六进制大写>
```

例如本机 WiFi STA MAC 为 `e0:72:a1:ea:01:d8`，则三处名称都是 **`ESPARK_EA01D8`**。

- 手机蓝牙扫描、WiFi 热点扫描、路由器后台的客户端列表，看到的名字完全一致，多台设备可以直观区分
- 名称由 `main/common/device_name.c` 的 `device_name_get()` 统一生成，前缀见 `DEVICE_NAME_PREFIX`
- STA 主机名在 `wifi_manager.c` 创建 netif 后、DHCP 启动前通过 `esp_netif_set_hostname()` 设置（改配置后需重连一次才生效）
- 蓝牙侧走标准 BluFi（服务 0xFFFF，特征 0xFF01/0xFF02）；热点密码仍取 `app_config` 的 `ap_pass`

## 状态灯与故障指示

右下角为状态灯区，故障时从最右 (15,15) 起向左点亮红灯，每类故障一盏，用于区分故障来源：

| 画面表现 | 含义 |
| -------- | ---- |
| 1 盏红灯 (15,15) | Wi-Fi 故障（连续 3 次连接失败） |
| 2 盏红灯 (15,15)+(14,15) | Wi-Fi 正常但天气接口故障，温度与天气图标沿用上一次成功数据 |
| 青色灯 | 天气请求进行中 |
| 蓝色灯 | 蓝牙已连接 |
| 蓝色或青色双闪两次 | 联网成功 / 天气刷新成功提示，随后回到基础状态 |
| 绿色呼吸灯 | 正常 |

天气失败后的恢复：失败不清除已显示的天气数据（沿用上一次成功值），仅点亮故障灯；天气窗口（10 分钟）内每分钟自动重试，窗口结束后若仍失败，继续每分钟重试，网络恢复后最多一分钟内清除故障灯并刷新数据。

## 固定定位配置（BluFi 自定义数据）

设备默认通过公网 IP 定位（`ipwho.is`，备用 `ipapi.co`）。部分运营商链路的 IP 定位结果与真实城市偏差很大（例如电信线路被定位到其他省份甚至境外），会导致天气温度明显不准。

可通过 BluFi 配网的自定义数据通道配置固定坐标（写入 NVS，永久生效，优先于 IP 定位）：

- `loc=30.2741,120.1551`：设置固定坐标（示例为杭州），保存后立即刷新天气
- `loc=auto`：清除固定坐标，恢复 IP 定位

使用支持 BluFi 自定义数据的手机 App（如 Espressif 的 EspBluFi）连接设备后，发送上述文本即可。

## 异常管理与自愈（P 编码）

所有故障统一登记到"异常寄存器表"（NVS 命名空间 `app_fault`），每条异常有 P 开头的固定编码、名称和等级。等级 1 的自愈时限按影响程度分档：

| 编码 | 名称 | 等级 | 触发条件 | 自愈时限 | 处理方式 |
| ---- | ---- | ---- | -------- | -------- | -------- |
| P001 | WEATHER | 1 | 天气接口持续不可用 | **1 小时** | 到点未恢复自动重启 |
| P002 | WIFI | 1 | Wi-Fi 连续 3 次连接失败 | **30 分钟** | 到点未恢复自动重启 |
| P003 | DISPLAY | 1 | 画面停滞超过 10 秒 | **10 分钟** | 到点未恢复自动重启 |
| P004 | STORAGE | 1 | 事件日志写入 NVS 失败 | **2 小时** | 到点未恢复自动重启 |
| P101 | RTC | 2 | DS3231 连续 10 次读取失败 | 不重启 | 长期挂载，仅记录 |
| P102 | RADAR | 2 | 雷达连续 60 秒无有效帧 | 不重启 | 长期挂载，仅记录 |
| P103 | LOCATION | 2 | 定位结果不在中国境内（疑似代理/VPN 出口） | 不重启 | 长期挂载，仅记录 |
| P104 | LOW_BATTERY | 2 | 低电量（预留，当前无电池供电） | 不重启 | 长期挂载，仅记录 |

- **等级 1**：异常在本次运行内持续超过对应时限仍未恢复时自动重启自愈，串口打印 `fault P003-DISPLAY active for 10 min; rebooting for self-healing`。
- **等级 2**：可长期挂载，只累计次数与首末次时间，不触发重启。
- 寄存器表在异常发生/恢复时写入 NVS，重启后保留历史统计；开机串口日志会打印完整表（`fault[i] Pxxx level=.. active=.. count=.. first=.. last=.. self_heal=10min/permanent`）。
- 自愈时限集中定义在 `main/common/app_fault.c` 的 `s_self_heal_min[]` 表中，按影响程度调整。

详细业务流程和点阵布局见项目外部文件：
`D:\Project\WiTime_Hardware\witime_design.drawio`

独立硬件引脚图位于项目外部：
`D:\Project\WiTime_Hardware\witime_esp32s3_pinout.drawio`

(See the README.md file in the upper level 'examples' directory for more information about examples.)

This is the simplest buildable example. The example is used by command `idf.py create-project`
that copies the project to user specified path and set it's name. For more information follow the [docs page](https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html#start-a-new-project)



## How to use example
We encourage the users to use the example as a template for the new projects.
A recommended way is to follow the instructions on a [docs page](https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html#start-a-new-project).

## Example folder contents

The project **sample_project** contains one source file in C language [main.c](main/main.c). The file is located in folder [main](main).

ESP-IDF projects are built using CMake. The project build configuration is contained in `CMakeLists.txt`
files that provide set of directives and instructions describing the project's source files and targets
(executable, library, or both). 

Below is short explanation of remaining files in the project folder.

```
├── CMakeLists.txt
├── main
│   ├── CMakeLists.txt
│   └── main.c
└── README.md                  This is the file you are currently reading
```
Additionally, the sample project contains Makefile and component.mk files, used for the legacy Make based build system. 
They are not used or needed when building with CMake and idf.py.
