# 显示调整同步记录

同步来源：[lmarzen/esp32-weather-epd](https://github.com/lmarzen/esp32-weather-epd)

本次同步对应天气项目 2026-09-05 的显示修改，内容如下：

| 来源文件 | 已同步内容 |
|---|---|
| `include/config.h` | `DISP_3C_B`、`GDEY075Z08`、`zh_CN`、摄氏度、日历/农历/节气/节假日开关 |
| `src/renderer.cpp` | GB2312 中文字体、中文文本宽度/高度测量、三色屏标准全刷波形、城市+日期单行标题 |
| `src/calendar.cpp` | 月历布局、周一开周、农历日期、节气、节日与休/班标记 |
| `include/calendar_photos.h` | 日历右侧照片/图像资源接口 |
| `include/calendar_weekdays.h` | 星期图形资源 |
| `src/display_utils.cpp` | ESP32-S3 显示启动与电源流程所需的兼容处理 |

ESP-IDF 融合工程不能直接编译 Arduino `renderer.cpp`，因此同步采用两层结构：

1. `include/weather_epd_display_config.h` 保存已经确认的显示策略和布局参数；
2. `arduino_port/` 保留天气项目的显示实现快照，供后续接入 Arduino-as-a-component 或逐步改写为 ESP-IDF 原生渲染器。

当前工程已通过 Arduino-ESP32 组件实际接入完整 GxEPD2 渲染器，并在 COM7 的
ESP32-S3-N16R8 上完成刷写验证；原生 `epd_gdey075z08.c` 仍保留为关闭桥接时的
后备实现。天气页使用 BME280 本地数据，配置 API key 后可使用 One Call 3.0
远程天气状态/图标。

第一阶段适配接口已完成：`weather_display.h` 提供稳定 C API，
`arduino_display_bridge.cpp` 提供 Arduino-ESP32 生命周期桥接；通过
`CONFIG_WEATHER_EPD_ARDUINO_RENDERER` 控制是否启用桥接，当前
`esp32s3-weather` 环境已启用，且不引入天气项目原有的 setup/loop、网络主循环
或深度睡眠逻辑。
