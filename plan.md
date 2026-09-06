# ESP32-S3 AirPlay Weather E-Paper 开发计划

## 总体架构

采用 **ESP-IDF 主工程 + Arduino 显示子组件** 的混合架构：

- ESP-IDF：AirPlay/RAOP、DLNA Renderer、网络配网、NTP、音频输出、USB Host。
- Arduino 显示子组件：GxEPD2、U8g2 中文字体、GDEY075Z08 渲染、天气页面、日历/农历/节气/节假日页面。
- 显示任务独立运行，通过队列/互斥锁与音频和网络任务隔离。
- 稳定后再逐步把显示驱动原生化为 ESP-IDF SPI/I²C/NVS 实现。

## 阶段一：Arduino 显示子组件（当前）

- [x] 建立 `weather_epd` 独立组件边界。
- [x] 同步天气项目最新显示策略：GDEY075Z08 三色红色强调、简体中文、GB2312、城市/日期单行标题、周一开周日历、农历、节气、节假日。
- [x] 保存最新 `renderer.cpp`、`calendar.cpp`、字体、图标和日历图片资源快照。
- [x] 引入 Arduino 显示依赖的可迁移快照目录 `components/weather_epd/arduino_port/`。
- [x] 在 ESP-IDF 工程中声明 Arduino-ESP32 作为组件依赖。
- [x] 建立 Arduino 显示快照与主工程之间的独立适配边界；快照默认不参与构建，避免影响 AirPlay 基线。
- [x] 提供稳定 C API：`weather_display_init`、`weather_display_show_weather`、`weather_display_show_calendar`、`weather_display_set_page`、`weather_display_power_off`。
- [x] 用实际 GDEY075Z08 硬件验证完整 LUT、黑/红双色刷新和中文字体显示（COM7 已刷写，EPD 完成全刷）。

阶段一实现说明：已完成组件依赖声明、快照隔离、Arduino 生命周期桥接和稳定 C API；
完整 GxEPD2 源文件暂不默认编译，待 Arduino-ESP32、GxEPD2、Adafruit GFX、
U8g2_for_Adafruit_GFX、Nongli 的组件依赖锁定后打开
`CONFIG_WEATHER_EPD_ARDUINO_RENDERER`。

## 阶段二：显示任务与数据管线

- [x] 建立独立 `weather_display_task`，默认十分钟刷新。
- [x] BME280 温湿度/气压读取和错误回退。
- [x] 增加 OpenWeatherMap One Call 3.0 可选数据入口（配置 API key 后启用）。
- [x] 天气参数网页化：通过 `/api/weather/config` 配置 API Key、经纬度、城市、启用状态和刷新周期，并使用 NVS 持久化；支持 `/api/weather/refresh` 立即刷新。
- [x] 天气页右侧接入 24 小时温度趋势和降雨趋势图；One Call 查询保留 hourly 数据并在远程更新后立即唤醒显示任务重绘。
- [x] 将 One Call 小时/多日预报完整映射到显示页面。
- [x] 将节假日网络数据映射到日历页面（农历/节气基础绘制已接入）。
- [ ] 使用 FreeRTOS 队列传递快照，使用显示互斥锁保护 SPI 总线。

## 阶段三：AirPlay/DLNA 与音频输出联调

- [ ] 验证 AirPlay RAOP 与 DLNA Renderer 长时间播放。
- [ ] 验证 PCM5102A I²S 输出和 USB Host UAC 耳放切换。
- [ ] 确认 GDEY075Z08 SPI 与 PCM5102A I²S GPIO 无冲突。
- [ ] 播放期间禁止阻塞式全屏刷新，必要时延后电子纸刷新。

## 阶段四：原生 ESP-IDF 显示驱动（后续优化）

- [ ] GxEPD2 替换为 ESP-IDF `spi_master` 原生驱动。
- [ ] Arduino `String`/`Preferences`/`Wire` 替换为标准 C++、NVS、`i2c_master`。
- [ ] 评估 U8g2 中文字体栅格化方案，减少固件体积和 RAM 占用。
- [ ] 完成显示组件脱离 Arduino 的构建和实机回归测试。

## 当前第一阶段交付标准

第一阶段完成后，ESP-IDF 主工程应能调用统一的显示 API；显示代码、字体和图标资源只在显示子组件内维护，不得重新引入天气项目的网络主循环或深度睡眠逻辑。

### 阶段一当前状态

组件依赖声明、显示快照隔离、Arduino 生命周期桥接和 C API 已完成。完整天气
renderer 的最终编译接入暂不标记完成，因为 GxEPD2、Adafruit GFX、
U8g2_for_Adafruit_GFX 和 Nongli 目前仍是 PlatformIO Arduino 库，尚未锁定为
ESP-IDF 组件依赖；这一步将在下一次依赖锁定后开启，不影响当前 AirPlay 基线。
# 2026-09-05 显示融合进度

- 已同步 `esp32-weather-epd` 的 `esp32_s3_n16r8` 引脚配置：GDEY075Z08 使用
  GPIO7/10/11/12/16/17，BME280 使用 GPIO8/9。
- 已将源工程的日历照片资源接入 ESP-IDF 原生 UC8179 驱动，避免依赖
  Arduino/GxEPD2 才能显示。
- 天气页已显示位置、状态、BME280 温度/湿度/气压和 PM2.5/PM10 字段;
- 日历页按当前月份绘制月历，当前日期以红色高亮，右侧显示照片缩略图；后台
  每 10 分钟自动轮换天气页和日历页。
- 已在 COM7 的 ESP32-S3-N16R8 上构建并刷写成功；串口确认 EPD 刷新、BME280、
  Wi-Fi、DLNA、AirPlay 和 PCM5102A I²S 初始化均正常。

### 天气网页配置接口

- `GET /api/weather/config`：读取启用状态、经纬度、城市、刷新周期和 API Key 是否已保存（Key 仅返回掩码）。
- `POST /api/weather/config`：保存 JSON 配置；API Key 留空表示保留原值，`clear_api_key:true` 可清除。
- `POST /api/weather/refresh`：立即发起一次 OpenWeatherMap 查询。
