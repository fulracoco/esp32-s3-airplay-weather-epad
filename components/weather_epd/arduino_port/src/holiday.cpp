/* Chinese public holiday client for esp32-weather-epd.
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

#include "holiday.h"

#include <cstdlib>
#include <cstring>

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "config.h"

namespace
{

constexpr uint32_t HOLIDAY_DATA_VERSION = 0x484F4C03;
constexpr char HOLIDAY_NVS_KEY[] = "holiday";
constexpr char HOLIDAY_RETRY_NVS_KEY[] = "holidayTry";
constexpr char HOLIDAY_RETRY_SOURCE_NVS_KEY[] = "holidaySrc";

uint32_t holiday_source_hash()
{
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < HOLIDAY_API_ENDPOINT.length(); ++i)
  {
    hash ^= static_cast<uint8_t>(HOLIDAY_API_ENDPOINT[i]);
    hash *= 16777619UL;
  }
  return hash;
}

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
  if (month == 2 && is_leap_year(year))
  {
    return 29;
  }
  return month >= 1 && month <= 12 ? month_days[month - 1] : 0;
}

int day_index(int year, int month, int day)
{
  if (day < 1 || day > days_in_month(year, month))
  {
    return -1;
  }
  int index = day - 1;
  for (int current_month = 1; current_month < month; ++current_month)
  {
    index += days_in_month(year, current_month);
  }
  return index;
}

bool load_cached_holidays(int year, int month, holiday_data_t &result)
{
  Preferences preferences;
  if (!preferences.begin(NVS_NAMESPACE, true))
  {
    return false;
  }

  bool loaded = false;
  if (preferences.getBytesLength(HOLIDAY_NVS_KEY) == sizeof(result))
  {
    preferences.getBytes(HOLIDAY_NVS_KEY, &result, sizeof(result));
    loaded = result.version == HOLIDAY_DATA_VERSION
             && result.source_hash == holiday_source_hash()
             && result.year == year
             && (result.coverage_month == 0
                 || result.coverage_month == month);
  }
  preferences.end();
  return loaded;
}

bool save_holidays(const holiday_data_t &data)
{
  Preferences preferences;
  if (!preferences.begin(NVS_NAMESPACE, false))
  {
    return false;
  }
  const size_t written = preferences.putBytes(HOLIDAY_NVS_KEY, &data,
                                               sizeof(data));
  preferences.end();
  return written == sizeof(data);
}

uint32_t calendar_date(const tm &time_info)
{
  return (time_info.tm_year + 1900) * 10000UL
         + (time_info.tm_mon + 1) * 100UL + time_info.tm_mday;
}

bool holiday_fetch_attempted_today(const tm &time_info)
{
  Preferences preferences;
  if (!preferences.begin(NVS_NAMESPACE, true))
  {
    return false;
  }
  const uint32_t last_attempt = preferences.getUInt(HOLIDAY_RETRY_NVS_KEY, 0);
  const uint32_t source_hash = preferences.getUInt(
    HOLIDAY_RETRY_SOURCE_NVS_KEY, 0);
  preferences.end();
  return last_attempt == calendar_date(time_info)
         && source_hash == holiday_source_hash();
}

void set_holiday_fetch_attempt(const tm &time_info, bool failed)
{
  Preferences preferences;
  if (!preferences.begin(NVS_NAMESPACE, false))
  {
    return;
  }
  if (failed)
  {
    preferences.putUInt(HOLIDAY_RETRY_NVS_KEY, calendar_date(time_info));
    preferences.putUInt(HOLIDAY_RETRY_SOURCE_NVS_KEY, holiday_source_hash());
  }
  else
  {
    preferences.remove(HOLIDAY_RETRY_NVS_KEY);
    preferences.remove(HOLIDAY_RETRY_SOURCE_NVS_KEY);
  }
  preferences.end();
}

bool parse_holidays(Stream &stream, int year, int month,
                    holiday_data_t &result)
{
  JsonDocument document;
  DeserializationError error = deserializeJson(document, stream);
  if (error)
  {
    Serial.println("Holiday API JSON error: " + String(error.c_str()));
    return false;
  }
  result = {};
  result.version = HOLIDAY_DATA_VERSION;
  result.source_hash = holiday_source_hash();
  result.year = year;

  JsonArray annual_days = document["days"].as<JsonArray>();
  if (!annual_days.isNull())
  {
    if ((document["year"] | year) != year)
    {
      Serial.println("Holiday JSON year does not match the request");
      return false;
    }
    for (JsonObject item : annual_days)
    {
      const char *date = item["date"] | "";
      if (strlen(date) != 10 || atoi(date) != year)
      {
        continue;
      }
      const int index = day_index(year, atoi(date + 5), atoi(date + 8));
      if (index >= 0)
      {
        const bool is_holiday = item["isOffDay"] | false;
        result.days[index] = is_holiday ? 1 : -1;
      }
    }
    result.coverage_month = 0;
    return true;
  }

  // Backward-compatible parsing for the API format used by J-Calendar.
  if ((document["code"] | -1) == 0)
  {
    JsonObject holidays = document["holiday"].as<JsonObject>();
    if (!holidays.isNull())
    {
      result.coverage_month = month;
      for (JsonPair item : holidays)
      {
        const char *key = item.key().c_str();
        const size_t key_length = strlen(key);
        if (key_length < 2)
        {
          continue;
        }
        const int day = atoi(key + key_length - 2);
        const int index = day_index(year, month, day);
        if (index >= 0)
        {
          const bool is_holiday = item.value()["holiday"] | false;
          result.days[index] = is_holiday ? 1 : -1;
        }
      }
      return true;
    }
  }

  Serial.println("Holiday API response has no supported holiday data");
  return false;
}

bool fetch_holidays(int year, int month, holiday_data_t &result)
{
  if (WiFi.status() != WL_CONNECTED)
  {
    return false;
  }

  String url = HOLIDAY_API_ENDPOINT;
  if (url.indexOf("{year}") >= 0 || url.indexOf("{month}") >= 0)
  {
    url.replace("{year}", String(year));
    url.replace("{month}", String(month));
  }
  else
  {
    while (url.endsWith("/"))
    {
      url.remove(url.length() - 1);
    }
    url += "/" + String(year) + "-" + String(month);
  }

  WiFiClientSecure client;
  // The endpoint is user-configurable and carries only public calendar data.
  // Certificate verification cannot use the OpenWeatherMap-specific CA.
  client.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(HTTP_CLIENT_TCP_TIMEOUT);
  http.setTimeout(HTTP_CLIENT_TCP_TIMEOUT);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  Serial.println("Holiday API request: " + url);
  if (!http.begin(client, url))
  {
    Serial.println("Unable to start holiday API request");
    return false;
  }

  const int response = http.GET();
  bool success = false;
  if (response == HTTP_CODE_OK)
  {
    success = parse_holidays(http.getStream(), year, month, result);
  }
  else
  {
    Serial.println("Holiday API HTTP error: " + String(response));
  }

  http.end();
  client.stop();
  return success;
}

} // namespace

bool load_or_fetch_holidays(const tm &time_info, holiday_data_t &result)
{
  const int year = time_info.tm_year + 1900;
  const int month = time_info.tm_mon + 1;
  result = {};

  if (load_cached_holidays(year, month, result))
  {
    Serial.println("Loaded holiday data from NVS");
    return true;
  }
  if (holiday_fetch_attempted_today(time_info))
  {
    Serial.println("Holiday API retry deferred until tomorrow");
    return false;
  }
  if (!fetch_holidays(year, month, result))
  {
    result = {};
    set_holiday_fetch_attempt(time_info, true);
    Serial.println("Holiday data unavailable; calendar will continue without it");
    return false;
  }

  if (!save_holidays(result))
  {
    Serial.println("Holiday data fetched but could not be cached");
  }
  else
  {
    set_holiday_fetch_attempt(time_info, false);
  }
  return true;
}

int8_t get_holiday_state(const holiday_data_t &data, int year, int month,
                         int day)
{
  if (data.version != HOLIDAY_DATA_VERSION || data.year != year
      || (data.coverage_month != 0 && data.coverage_month != month))
  {
    return 0;
  }
  const int index = day_index(year, month, day);
  return index >= 0 ? data.days[index] : 0;
}
