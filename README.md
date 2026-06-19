# ESP32-S3 OBD HUD Firmware

这是 ESP32-S3 OBD HUD 固件工程，基于 ESP-IDF 和 LVGL，目标芯片为 ESP32-S3。

## 当前功能

- ESP-IDF 工程入口
- ST77916 LCD 初始化
- LVGL 基础显示循环
- 开机 Logo PNG 资源
- 蓝牙 OBD 设备扫描、选择和连接
- OBD 数据解析与仪表页面显示
- OBD 运行时数据缓存，RPM / speed 平滑输出
- Dashboard 圆弧式仪表 UI，底部水温弧支持高温变色
- Clock 时钟页面
- IMU / G-Force Max 页面
- GPS UART 状态页面
- GPS UART 大号速度显示，使用真实大字号字体渲染
- PCF85063 RTC 驱动
- QMI8658 六轴驱动
- WiFi + NTP 校时支持

## 环境

本机 ESP-IDF 路径：

```bash
/Users/bk/.espressif/v5.5.4/esp-idf
```

普通 shell 默认没有加载 `idf.py`，构建前需要先激活 ESP-IDF：

```bash
source /Users/bk/.espressif/v5.5.4/esp-idf/export.sh
```

已验证版本：

```text
ESP-IDF v5.5.4
Python 3.13.14
```

## 配置状态

- 目标芯片：`esp32s3`
- Flash：`16MB`
- 分区表：`partitions.csv`
- LVGL：16-bit color，启用 byte swap
- Bluetooth：启用 NimBLE，未启用 Bluedroid
- PSRAM：当前未启用
- WiFi/NTP：功能已接入，默认未配置 SSID，不会主动联网

PSRAM 相关配置：

```text
# CONFIG_SPIRAM is not set
# CONFIG_ESP32S3_SPIRAM_SUPPORT is not set
```

## Build

```bash
source /Users/bk/.espressif/v5.5.4/esp-idf/export.sh
idf.py build
```

生成固件：

```text
build/esp32s3_obd.bin
```

## 页面操作

- 开机 Logo 自动进入 Clock 页面。
- Clock / IMU / Dashboard / GPS UART / OBD Details 支持左/右滑循环切换。
- OBD Details 上滑进入 Bluetooth。
- Clock 上/下滑切换主题；双击进入小时/分钟设置；设置模式下上/下滑调整时间。
- IMU 双击复位校正；每次进入 IMU 页面也会自动复位校正。
- Bluetooth 右滑返回 OBD Details；连接成功后自动回到 Dashboard。
- OBD Details 使用与 GPS UART 同款绿色外圈。

## Newfeatures 移植内容

已从 `../project01_newfeatures` 移植：

- `Clock / IMU / UART` 页面
- `PCF85063` RTC 驱动
- `QMI8658` 六轴驱动
- GPS UART 接收与 NMEA/UBX 解析
- WiFi/NTP 到 RTC 的校时逻辑
- 页面字体资源
- `FontTypoderSize140` 大号数字字体，用于 GPS UART 速度显示

WiFi/NTP 默认不启用。需要联网校时时，在构建前给 `NEWFEATURES_WIFI_SSID` 和
`NEWFEATURES_WIFI_PASS` 提供非空值，或后续改成 menuconfig 配置项。

## OBD Cache 移植内容

已从 `../project02_bd_gauge` 移植 OBD runtime cache：

- 新增 `components/esp32s3_obd/obd_data_cache.c`
- 新增 `components/esp32s3_obd/obd_data_cache.h`
- BLE OBD PID 解析成功后写入 cache
- `obd_ble_get_snapshot()` 返回时从 cache 读取数据
- RPM / speed getter 使用一阶滤波，显示数值缓升缓降
- 冷却液温度、进气温度、发动机负荷、TPS、电压、油量、机油温进入统一 cache
- BLE 新连接开始时清空 cache，避免上一次连接的数据残留

未移植 `project02_bd_gauge` 中依赖 `nvs_storage` 的里程统计任务；当前工程暂只接入实时 OBD 数据缓存。

## Dashboard UI

- Dashboard 使用圆弧式仪表布局：顶部/右侧大圆弧表示 RPM 区域，右侧红色段表示高转速区。
- RPM 数字 `0-10` 布置在大圆弧带上。
- RPM 使用平滑后的 cache 数据驱动红色楔形指针，指针按 `0-10000 RPM` 映射到大圆弧对应位置。
- 速度使用 `FontTypoderSize100` 大号数字居中显示，下方显示 `km/h`。
- 底部小圆弧绑定 OBD 冷却液温度：水温 `> 95°C` 时变红，水温 `<= 95°C` 或暂无数据时保持绿色。
- 水温数字显示在底部小圆弧附近，无数据时显示 `00'C`。

## Flash

```bash
source /Users/bk/.espressif/v5.5.4/esp-idf/export.sh
idf.py -p PORT flash monitor
```

将 `PORT` 替换为实际串口设备。

## 近期修复

- 修复蓝牙页面点击 `scan all` 后触发 `ble_scan_restart` 栈溢出重启的问题。
- `ble_scan_restart` 任务栈从 `2048` 调整为 `4096`。
- 扫描重启任务创建失败时会清理 pending 状态，避免扫描状态卡住。
- 从 `project01_newfeatures` 移植 Clock、IMU、GPS UART、RTC、NTP 校时等功能。
- 从 `project02_bd_gauge` 移植 OBD runtime cache，Dashboard 速度/RPM 读取平滑后的缓存值。
- Dashboard 速度数字改为和 RPM / TEMP 一样的固定宽度居中 label 渲染。
- GPS UART 页面速度数字改用真实 `FontTypoderSize140` 大字号字体，避免 `transform_zoom` 导致不渲染。
- GPS UART 速度无数据时显示 `00`，避免大字号数字字体缺少 `-` 时出现缺字方框。
- 页面顺序调整为 Clock / IMU / Dashboard / GPS UART / OBD Details。
- 蓝牙入口从 Dashboard 上滑改为 OBD Details 上滑，Bluetooth 右滑返回 OBD Details。
- Dashboard 大圆弧加宽并改为实色显示，RPM 数字居中落在圆弧带上。
- Dashboard 底部小圆弧与冷却液温度绑定，水温超过 `95°C` 时变红。
- Dashboard RPM 指针改为尾粗前尖的红色楔形多边形，由平滑后的 RPM cache 数据驱动。
