# ESP32-S3 AirPlay Weather E-Paper

融合 `esp32-weather-epd` 与 `airplay-esp32` 的 ESP-IDF 工程基线，目标硬件：

- ESP32-S3-N16R8（16 MB Flash / 8 MB OPI PSRAM）
- GDEY075Z08 7.5 英寸黑/红双色墨水屏
- BME280（3.3 V I²C）
- 外置 I²S DAC，或 ESP32-S3 USB Host UAC 耳放

详细接线请参阅 [硬件接线定义.md](./硬件接线定义.md)。
显示调整同步记录请参阅 [components/weather_epd/DISPLAY_SYNC.md](./components/weather_epd/DISPLAY_SYNC.md)。

## 构建

```text
pio run -e esp32s3-weather
pio run -e esp32s3-weather-usb-host
```

也可以使用原生 ESP-IDF：

```text
idf.py -DSDKCONFIG_DEFAULTS="config/sdkconfig.defaults;config/sdkconfig.defaults.esp32s3;config/sdkconfig.defaults.esp32s3-weather" build
```

AirPlay RAOP、DLNA Renderer、网络配置页面和音频输出均沿用原项目。天气组件位于 `components/weather_epd`，以独立 FreeRTOS 任务进行传感器采样和墨水屏刷新，避免阻塞音频实时任务。显示配置已同步天气项目最新调整：中文字体、三色红色强调色、城市/日期单行标题，以及周一开周的农历/节气/节假日日历页。显示调用统一通过 `weather_display.h` 的 C API 完成。

## 当前阶段

已完成工程融合、ESP32-S3 板级配置、服务启动接线、传感器/屏幕驱动接口和 USB Host 构建 profile。GDEY075Z08 的最终 LUT 刷屏序列、BME280 校准计算、天气 API/农历节假日页面仍需在实际硬件和网络凭据下联调后启用。
