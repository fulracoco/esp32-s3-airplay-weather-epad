# ESP32-S3 AirPlay 1 Weather E-Paper

这是一个面向 **ESP32-S3-N16R8** 的融合固件：把 **AirPlay 1 音频接收**、
**I²S/USB DAC 音频输出**、**天气墨水屏**和**日历**放在同一块板上。

它不是通用的多芯片示例工程，当前硬件目标只有：

- ESP32-S3-N16R8（16 MB Flash、8 MB OPI PSRAM）
- GDEY075Z08 7.5 英寸黑/红双色电子纸（通过 DESPI-C02 或兼容驱动板）
- BME280 温湿度/气压传感器
- 二选一的音频输出：外置 PCM5102A 等 I²S DAC，或 USB Host UAC DAC/耳放

## 项目定位

```text
AirPlay 1 (RAOP/ALAC over UDP)
                 │
                 ▼
       ESP32-S3-N16R8 音频管线
          ┌──────┴──────┐
          ▼             ▼
      I²S DAC       USB Host UAC DAC

      独立 FreeRTOS 显示任务
          ├─ 天气、预报、空气质量
          └─ 公历/农历/节气/节假日/调休日历
                 │
                 ▼
             GDEY075Z08
```

音频接收和墨水屏刷新使用独立任务。电子纸的慢速刷新、天气请求或日历渲染
不会阻塞 AirPlay 播放。

## 主要功能

### AirPlay 1 音频

- AirPlay 1 / RAOP 接收，使用实时 **ALAC over UDP** 音频路径
- 支持 iPhone、iPad、Mac 以及其他 AirPlay 1 发送端
- 低延迟 PCM 解码、时钟同步和音量控制
- 当前融合 profile 固定优先 AirPlay 1；不以 AirPlay 2 AAC-ELD 为目标

### 两种 DAC 输出

- **I²S DAC**：连接 PCM5102A 等外置标准 I²S DAC
- **USB DAC**：ESP32-S3 原生 USB OTG 作为 Host，驱动 UAC 1.0 外置 DAC/USB 耳放
- 两种输出共用 AirPlay 音频管线，可通过 PlatformIO 环境或 `menuconfig` 选择
- USB Host 输出要求开发板 USB 口能够为外设提供 5 V VBUS；部分板卡需要 OTG
  转接头或有源 USB Hub

### 天气墨水屏

- GDEY075Z08 黑/红双色显示，中文天气图标和字体
- BME280 本地温度、湿度、气压读数
- 可选 OpenWeatherMap One Call 3.0：当前天气、逐小时/逐日预报、降雨概率
- 可选空气质量数据（AQI、PM2.5、PM10、NO₂、O₃ 等）
- 城市、经纬度、刷新周期、时区、NTP 和节假日接口均可从 Web 页面配置
- API Key 等配置保存在设备 NVS，不写入源代码

### 日历与扩展页面

- 公历日期、星期、农历日期和生肖
- 二十四节气、法定节假日和调休/工作日标记
- 天气页与日历页自动轮播，也可以请求立即刷新日历
- Web 页面支持上传日历照片，裁剪、缩放、阈值、对比度和抖动方式可调

## 硬件接线

完整接线请看 [硬件接线定义.md](硬件接线定义.md)。融合 profile 的关键引脚如下：

| 外设 | ESP32-S3-N16R8 GPIO |
| --- | --- |
| GDEY075Z08 SPI（SCK/MOSI/CS/DC/RST/BUSY） | 12 / 11 / 10 / 17 / 16 / 7 |
| BME280 SDA/SCL | 8 / 9 |
| PCM5102A I²S（BCK/DOUT/LRCK/MCLK） | 38 / 40 / 39 / 41 |

I²S 引脚经过重新规划，避开了墨水屏 SPI 和 BME280 I²C 总线。使用 USB DAC
时不需要连接 PCM5102A，但仍应按表格连接墨水屏和 BME280。

## 构建与烧录

需要 PlatformIO、ESP-IDF 5.5+ 和递归克隆的子模块：

```bash
git clone --recursive https://github.com/fulracoco/esp32-s3-airplay-weather-epad.git
cd esp32-s3-airplay-weather-epad
```

### I²S DAC 版本

```bash
pio run -e esp32s3-weather -t upload
pio run -e esp32s3-weather -t uploadfs   # 写入 Web 配置页面和 SPIFFS 数据
pio run -e esp32s3-weather -t monitor
```

### USB Host DAC 版本

```bash
pio run -e esp32s3-weather-usb-host -t upload
pio run -e esp32s3-weather-usb-host -t uploadfs
pio run -e esp32s3-weather-usb-host -t monitor
```

两个环境都只针对 ESP32-S3-N16R8。修改 `config/` 默认值后，请删除根目录缓存的
`sdkconfig.<env>` 再重新构建。

## 首次配置

首次启动后连接设备创建的配置 Wi-Fi，打开设备提示的 Web 地址，配置：

1. Wi-Fi 和设备名称；
2. 音频输出（I²S DAC 或 USB Host DAC）；
3. 城市、经纬度和 OpenWeatherMap API Key；
4. 时区、NTP、天气刷新间隔和节假日接口。

不配置天气 API Key 时，设备仍可显示 BME280 本地数据和日历页面。不要把真实的
Wi-Fi 密码、API Key、内网地址或个人照片提交到 Git 仓库。

## 上游项目与引用说明

本项目在 ESP-IDF 中整合并改写了下列开源项目的代码、设计思路或数据处理方式。
请同时遵守各上游仓库的许可证、版权和署名要求；相关源文件保留原始声明。

- [rbouteiller/airplay-esp32](https://github.com/rbouteiller/airplay-esp32)：AirPlay/RAOP
  接收基础、RTSP/HAP 处理、网络配置和音频输出框架。本项目在其基础上收敛为
  ESP32-S3-N16R8 的 AirPlay 1 音频接收 profile，并加入 I²S/USB Host DAC 选择。
- [tsl0922/EPD-nRF5](https://github.com/tsl0922/EPD-nRF5)：电子纸日历的页面组织和
  交互参考，包含农历、节气、节假日及调休日等显示思路。
- [lmarzen/esp32-weather-epd](https://github.com/lmarzen/esp32-weather-epd)：天气页
  渲染、OpenWeatherMap 配置、BME280 数据、天气图标和 GDEY075Z08 适配参考。

另外，显示组件中包含 GxEPD2、U8g2、Adafruit GFX/BusIO、ArduinoJson 和 Nongli
等依赖；各自的许可证文件位于对应组件目录。

## 许可证与免责声明

本仓库的顶层许可证和上游组件许可证同时适用；如有冲突，以相关组件的原始许可证
和版权声明为准。项目与 Apple Inc. 无隶属关系，不保证兼容未来的系统版本，按现状
提供，不承担因使用本固件造成的损失。
