#pragma once

/* Display profile synchronized from the latest esp32-weather-epd renderer. */
#define WEATHER_EPD_PANEL_GDEY075Z08 1
#define WEATHER_EPD_PANEL_TRICOLOR   1
#define WEATHER_EPD_ACCENT_COLOR_RED 1
#define WEATHER_EPD_WIDTH            800
#define WEATHER_EPD_HEIGHT           480

/* Chinese UI profile. */
#define WEATHER_EPD_LOCALE_ZH_CN     1
#define WEATHER_EPD_FONT_GB2312      1
#define WEATHER_EPD_FONT_SIZE_PX     16

/* Calendar page options. */
#define WEATHER_EPD_DISPLAY_CALENDAR 1
#define WEATHER_EPD_WEEK_START_MONDAY 1
#define WEATHER_EPD_DISPLAY_LUNAR    1
#define WEATHER_EPD_DISPLAY_SOLAR_TERMS 1
#define WEATHER_EPD_DISPLAY_HOLIDAYS 1

/* Latest renderer places city and date on one larger line. */
#define WEATHER_EPD_LOCATION_DATE_SINGLE_LINE 1
#define WEATHER_EPD_LOCATION_DATE_BASELINE   30

/* Source-project formatting choices retained for the native renderer port. */
#define WEATHER_EPD_DATE_FORMAT_ZH_CN        "%Y年%m月%e日 %a"
#define WEATHER_EPD_TIME_FORMAT_24H          1
#define WEATHER_EPD_TEMPERATURE_CELSIUS      1
#define WEATHER_EPD_ACCENT_COLOR             0xF800 /* GxEPD_RED */
