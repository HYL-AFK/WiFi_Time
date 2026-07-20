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
