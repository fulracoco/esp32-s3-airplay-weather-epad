# Arduino 显示移植快照

此目录保存从 `esp32-weather-epd/platformio` 同步的显示实现和资源：

- `src/renderer.cpp`：GDEY075Z08 三色天气页面、中文字体和单行标题
- `src/calendar.cpp`：周一开周、农历、节气、节假日日历页
- `src/display_utils.cpp`：显示工具和电源流程
- `include/`：显示配置、locale、图标、字体、日历图形和图片资源

快照不直接参与 ESP-IDF 主工程编译，避免引入 Arduino 天气项目的网络循环、
深度睡眠和全局配置。主工程通过上层 `weather_display.h` C API 调用显示服务。

启用完整 renderer 前，需要将以下库锁定为 Arduino-ESP32 可用依赖：

1. Arduino-ESP32 3.x
2. GxEPD2 1.6.x
3. Adafruit GFX / Adafruit BusIO
4. U8g2_for_Adafruit_GFX
5. Nongli

