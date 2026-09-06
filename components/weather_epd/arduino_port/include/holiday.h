/* Chinese public holiday declarations for esp32-weather-epd.
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

#ifndef __HOLIDAY_H__
#define __HOLIDAY_H__

#include <Arduino.h>
#include <time.h>

struct holiday_data_t
{
  uint32_t version;
  uint32_t source_hash;
  uint16_t year;
  // 0 means the entire year is present. Other values represent a legacy
  // month-only response.
  uint8_t coverage_month;
  uint8_t reserved;
  int8_t days[366]; // 1 = holiday, -1 = adjusted workday, 0 = normal
};

bool load_or_fetch_holidays(const tm &time_info, holiday_data_t &result);
int8_t get_holiday_state(const holiday_data_t &data, int year, int month,
                         int day);

#endif
