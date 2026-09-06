/* Month calendar renderer for esp32-weather-epd.
 * Copyright (C) 2026  esp32-weather-epd contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "calendar.h"
#include "calendar_photo_store.h"
#include "calendar_weekdays.h"

#include <Arduino.h>
#include <time.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <nongli.h>

#include "_strftime.h"
#include "config.h"
#include "holiday.h"
#include "renderer.h"

#include FONT_HEADER

namespace
{

// The calendar is rendered as a dedicated full-screen page.
constexpr int CALENDAR_X = 14;
constexpr int CALENDAR_RIGHT = 408;
constexpr int CALENDAR_TOP = 4;
constexpr int CALENDAR_BOTTOM_MARGIN = 35;
constexpr int TITLE_HEIGHT = 35;
constexpr int WEEKDAY_HEIGHT = 26;
constexpr int WEEK_ROWS = 6;
constexpr int PHOTO_WIDTH = CALENDAR_PHOTO_WIDTH;
constexpr int PHOTO_HEIGHT = CALENDAR_PHOTO_HEIGHT;
constexpr int PHOTO_X = 425;
constexpr int PHOTO_Y = 4;

U8G2_FOR_ADAFRUIT_GFX chinese_fonts;
bool chinese_fonts_initialized = false;

#if defined(DISP_3C_B) || defined(DISP_7C_F)
constexpr uint16_t CALENDAR_ACCENT_COLOR = ACCENT_COLOR;
#else
constexpr uint16_t CALENDAR_ACCENT_COLOR = GxEPD_BLACK;
#endif

bool is_leap_year(int year)
{
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int days_in_month(int year, int month)
{
  static const uint8_t month_days[] =
  {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
  };

  if (month == 1 && is_leap_year(year))
  {
    return 29;
  }
  return month_days[month];
}

int weekday_column(int weekday)
{
  return (weekday - CALENDAR_WEEK_START + 7) % 7;
}

bool is_weekend(int weekday)
{
  return weekday == 0 || weekday == 6;
}

int day_of_year(int year, int month, int day)
{
  int result = day;
  for (int current_month = 0; current_month < month; ++current_month)
  {
    result += days_in_month(year, current_month);
  }
  return result;
}

String solar_festival_label(int month, int day)
{
  const int date = month * 100 + day;
  switch (date)
  {
    case 101:  return "元旦";
    case 501:  return "劳动节";
    case 601:  return "儿童节";
    case 1001: return "国庆节";
    default:   return "";
  }
}

String lunar_day_label(int lunar_day)
{
  static const char *tens[] = { "初", "十", "廿", "卅" };
  static const char *digits[] =
  {
    "十", "一", "二", "三", "四", "五", "六", "七", "八", "九"
  };

  if (lunar_day == 10)
  {
    return "初十";
  }
  if (lunar_day == 20)
  {
    return "二十";
  }
  if (lunar_day == 30)
  {
    return "三十";
  }
  if (lunar_day < 1 || lunar_day > 30)
  {
    return "";
  }
  return String(tens[lunar_day / 10]) + digits[lunar_day % 10];
}

String lunar_month_label(int lunar_month, bool is_leap_month)
{
  static const char *months[] =
  {
    "", "正", "二", "三", "四", "五", "六",
    "七", "八", "九", "十", "冬", "腊"
  };
  if (lunar_month < 1 || lunar_month > 12)
  {
    return "";
  }
  return String(is_leap_month ? "闰" : "") + months[lunar_month] + "月";
}

String lunar_festival_label(int lunar_month, int lunar_day)
{
  const int date = lunar_month * 100 + lunar_day;
  switch (date)
  {
    case 101:  return "春节";
    case 115:  return "元宵节";
    case 505:  return "端午节";
    case 707:  return "七夕";
    case 815:  return "中秋节";
    case 909:  return "重阳节";
    case 1208: return "腊八节";
    default:   return "";
  }
}

String calendar_annotation(int year, int month, int day, int lunar_date,
                           const int *solar_terms)
{
  String label = solar_festival_label(month + 1, day);
  if (!label.isEmpty())
  {
    return label;
  }

#if DISPLAY_SOLAR_TERMS
  const int accumulated_day = day_of_year(year, month, day);
  for (int i = 0; i < 24; ++i)
  {
    if (solar_terms[i] == accumulated_day)
    {
      return nl_jq_text[i];
    }
  }
#else
  (void)solar_terms;
#endif

#if DISPLAY_LUNAR_CALENDAR
  const bool is_leap_month = lunar_date < 0;
  lunar_date = abs(lunar_date);
  const int lunar_month = lunar_date / 100;
  const int lunar_day = lunar_date % 100;

  label = lunar_festival_label(lunar_month, lunar_day);
  if (!label.isEmpty())
  {
    return label;
  }
  if (lunar_day == 1)
  {
    return lunar_month_label(lunar_month, is_leap_month);
  }
  return lunar_day_label(lunar_day);
#else
  (void)lunar_date;
  return "";
#endif
}

void draw_chinese_centered(int x, int y, int width, const String &text,
                           uint16_t color)
{
  chinese_fonts.setForegroundColor(color);
  // Use the full GB2312 set here: lunar dates, solar terms and holiday names
  // need characters outside the compact weekday subset.
  chinese_fonts.setFont(u8g2_font_wqy16_t_gb2312);
  const int text_width = chinese_fonts.getUTF8Width(text.c_str());
  chinese_fonts.drawUTF8(x + (width - text_width) / 2, y, text.c_str());
}

void draw_holiday_marker(int x, int y, int cell_width, int8_t holiday_state)
{
  if (holiday_state == 0)
  {
    return;
  }

  const uint16_t color = holiday_state > 0
                         ? CALENDAR_ACCENT_COLOR : GxEPD_BLACK;
  if (cell_width >= 50)
  {
    chinese_fonts.setForegroundColor(color);
    chinese_fonts.setFont(u8g2_font_wqy12_t_chinese1);
    const char *marker = holiday_state > 0 ? "休" : "班";
    chinese_fonts.drawUTF8(x + cell_width - 13, y + 13, marker);
  }
  else if (holiday_state > 0)
  {
    display.fillCircle(x + cell_width - 6, y + 6, 3, color);
  }
  else
  {
    display.drawRect(x + cell_width - 9, y + 3, 6, 6, color);
  }
}

} // namespace

void draw_calendar_photos()
{
  const int left = PHOTO_X + (DISP_WIDTH - PHOTO_X - PHOTO_WIDTH) / 2;
  static uint8_t uploaded_photo[CALENDAR_PHOTO_BYTES];
  const int selected = calendar_photo_store_selected();
  if (selected < 0 ||
      calendar_photo_store_load((uint8_t)selected, uploaded_photo,
                                sizeof(uploaded_photo), nullptr) != ESP_OK)
  {
    return;
  }
  display.drawBitmap(left, PHOTO_Y, uploaded_photo, PHOTO_WIDTH, PHOTO_HEIGHT,
                     GxEPD_BLACK);
}

void draw_calendar(const tm &time_info, const holiday_data_t &holiday_data)
{
  const int calendar_right = CALENDAR_RIGHT;
  const int calendar_bottom = DISP_HEIGHT - CALENDAR_BOTTOM_MARGIN;
  const int calendar_width = calendar_right - CALENDAR_X;
  const int cell_width = calendar_width / 7;
  const int grid_width = cell_width * 7;
  const int grid_left = calendar_right - grid_width;
  const int weekday_top = CALENDAR_TOP + TITLE_HEIGHT;
  const int grid_top = weekday_top + WEEKDAY_HEIGHT;
  const int cell_height = (calendar_bottom - grid_top) / WEEK_ROWS;
  const int grid_bottom = grid_top + (cell_height * WEEK_ROWS);

  if (!chinese_fonts_initialized)
  {
    chinese_fonts.begin(display);
    chinese_fonts_initialized = true;
  }
  chinese_fonts.setFontMode(1);
  chinese_fonts.setFontDirection(0);
  chinese_fonts.setBackgroundColor(GxEPD_WHITE);

  // The former location/date header is intentionally omitted on this page.
  // Use that otherwise-empty strip for a row of monochrome photo thumbnails.
  draw_calendar_photos();

  const String title = String(time_info.tm_year + 1900) + "年"
                     + String(time_info.tm_mon + 1) + "月";
  draw_chinese_centered(grid_left, CALENDAR_TOP + 25, grid_width, title,
                        CALENDAR_ACCENT_COLOR);

  display.setFont(&FONT_8pt8b);
  for (int column = 0; column < 7; ++column)
  {
    const int weekday = (CALENDAR_WEEK_START + column) % 7;
    const int x = grid_left + column * cell_width;
    const uint16_t header_color = is_weekend(weekday)
                                    ? CALENDAR_ACCENT_COLOR : GxEPD_BLACK;

    display.fillRect(x, weekday_top, cell_width, WEEKDAY_HEIGHT, header_color);

#if DISPLAY_LUNAR_CALENDAR
    static const uint8_t *weekday_bitmaps[] =
    {
      weekday_0, weekday_1, weekday_2, weekday_3,
      weekday_4, weekday_5, weekday_6
    };
    display.drawBitmap(x + (cell_width - 24) / 2, weekday_top + 1,
                       weekday_bitmaps[column], 24, 24, GxEPD_WHITE);
#else
    tm weekday_time = {};
    weekday_time.tm_wday = weekday;
    char weekday_label[12] = {};
    _strftime(weekday_label, sizeof(weekday_label), "%a", &weekday_time);
    drawString(x + cell_width / 2, weekday_top + 18, weekday_label, CENTER,
               GxEPD_WHITE);
#endif
  }

  for (int row = 0; row <= WEEK_ROWS; ++row)
  {
    const int y = grid_top + row * cell_height;
    display.drawLine(grid_left, y, grid_left + grid_width, y, GxEPD_BLACK);
  }
  for (int column = 0; column <= 7; ++column)
  {
    const int x = grid_left + column * cell_width;
    display.drawLine(x, grid_top, x, grid_bottom, GxEPD_BLACK);
  }

  tm first_day = time_info;
  first_day.tm_mday = 1;
  first_day.tm_hour = 12;
  first_day.tm_min = 0;
  first_day.tm_sec = 0;
  first_day.tm_isdst = -1;
  mktime(&first_day);

  const int year = time_info.tm_year + 1900;
  const int month = time_info.tm_mon;
  const int first_column = weekday_column(first_day.tm_wday);
  const int total_days = days_in_month(year, month);

  int lunar_dates[31] = {};
  int solar_terms[24] = {};
#if DISPLAY_LUNAR_CALENDAR
  if (year >= 1902 && year <= 2099)
  {
    nl_month_days(year, month + 1, lunar_dates);
  }
#endif
#if DISPLAY_SOLAR_TERMS
  if (year >= 1900 && year <= 2099)
  {
    nl_year_jq(year, solar_terms);
  }
#endif

  display.setFont(&FONT_12pt8b);
  for (int day = 1; day <= total_days; ++day)
  {
    const int index = first_column + day - 1;
    const int row = index / 7;
    const int column = index % 7;
    const int weekday = (CALENDAR_WEEK_START + column) % 7;
    const int x = grid_left + column * cell_width;
    const int y = grid_top + row * cell_height;
    const int8_t holiday_state = get_holiday_state(
      holiday_data, year, month + 1, day);
    uint16_t day_color = is_weekend(weekday)
                         ? CALENDAR_ACCENT_COLOR : GxEPD_BLACK;
    if (holiday_state > 0)
    {
      day_color = CALENDAR_ACCENT_COLOR;
    }
    else if (holiday_state < 0)
    {
      day_color = GxEPD_BLACK;
    }

    const int text_y = y + (cell_height >= 48 ? 23 : 18);
    drawString(x + cell_width / 2, text_y, String(day), CENTER, day_color);
    draw_holiday_marker(x, y, cell_width, holiday_state);

    const String annotation = calendar_annotation(
      year, month, day, lunar_dates[day - 1], solar_terms);
    if (!annotation.isEmpty())
    {
      draw_chinese_centered(x, y + cell_height - 5, cell_width, annotation,
                            day_color);
    }

    if (day == time_info.tm_mday)
    {
      display.drawRoundRect(x + 3, y + 3, cell_width - 6, cell_height - 6,
                            5, CALENDAR_ACCENT_COLOR);
      display.drawRoundRect(x + 5, y + 5, cell_width - 10, cell_height - 10,
                            4, CALENDAR_ACCENT_COLOR);
    }
  }
}
