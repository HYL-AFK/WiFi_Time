# WiTime vX.Y.Z
## 版本信息

- 固件版本：`X.Y.Z`
- Git 提交：`<short-commit>`
- 芯片：ESP32-S3
- Flash：4MB

## 本次变更

- 待填写

## 烧录方式

1. 解压完整固件包。
2. 根据 `flash_args.txt` 的地址烧录 `bootloader.bin`、`partition-table.bin` 和应用固件。
3. 烧录后查看串口启动日志，确认项目名为 `WiTime`。

## 硬件要求

- WS2812 DIN -> GPIO10
- DS3231 SDA -> GPIO8
- DS3231 SCL -> GPIO9
- DS3231 与 ESP32、WS2812 共地

## 校验

使用 `SHA256SUMS.txt` 校验发布包文件完整性。
