idf_component_register(
    SRCS "weather_epd.c" "weather_display.c" "bme280.c" "epd_gdey075z08.c"
         "calendar_photo_store.c" "calendar_photo_ble.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_i2c esp_driver_spi esp_timer esp_wifi esp_netif esp_http_client json nvs_flash mbedtls
)

if(CONFIG_BT_ENABLED)
  idf_component_get_property(_weather_epd_component_lib COMPONENT_LIB)
  target_link_libraries(${COMPONENT_LIB} PRIVATE idf::bt)
endif()

if(CONFIG_WEATHER_EPD_ARDUINO_RENDERER)
  target_sources(${COMPONENT_LIB} PRIVATE
    "arduino_display_bridge.cpp"
    "arduino_port/src/renderer.cpp"
    "arduino_port/src/calendar.cpp"
    "arduino_port/src/display_utils.cpp"
    "arduino_port/src/conversions.cpp"
    "arduino_port/src/holiday.cpp"
    "arduino_port/src/locale.cpp"
    "arduino_port/src/_strftime.cpp"
    "arduino_port/src/config.cpp"
    "arduino_port/lib/Adafruit GFX Library/Adafruit_GFX.cpp"
    "arduino_port/lib/Adafruit GFX Library/Adafruit_SPITFT.cpp"
    "arduino_port/lib/Adafruit GFX Library/Adafruit_GrayOLED.cpp"
    "arduino_port/lib/Adafruit GFX Library/glcdfont.c"
    "arduino_port/lib/U8g2_for_Adafruit_GFX/src/U8g2_for_Adafruit_GFX.cpp"
    "arduino_port/lib/U8g2_for_Adafruit_GFX/src/u8g2_fonts.c"
    "arduino_port/lib/GxEPD2/src/GxEPD2_EPD.cpp"
    "arduino_port/lib/GxEPD2/src/gdey3c/GxEPD2_750c_GDEY075Z08.cpp"
    "arduino_port/include/nongli/nongli.cpp"
    "arduino_port/lib/aqi/aqi.c")
  target_include_directories(${COMPONENT_LIB} PRIVATE
    "arduino_port/include"
    "arduino_port/lib/GxEPD2/src"
    "arduino_port/lib/Adafruit GFX Library"
    "arduino_port/lib/Adafruit GFX Library/Fonts"
    "arduino_port/lib/Adafruit BusIO"
    "arduino_port/lib/U8g2_for_Adafruit_GFX/src"
    "arduino_port/lib/Nongli"
    "arduino_port/lib/ArduinoJson"
    "arduino_port/lib/aqi"
    "arduino_port/include/nongli")
  target_link_libraries(${COMPONENT_LIB} PRIVATE idf::espressif__arduino-esp32)
endif()
