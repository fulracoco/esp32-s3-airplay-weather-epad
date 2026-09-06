/* Startup diagnostics for esp32-weather-epd.
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

#ifndef STARTUP_DIAGNOSTICS_H
#define STARTUP_DIAGNOSTICS_H

#ifdef STARTUP_DIAGNOSTICS
  #include <Arduino.h>
  #include <esp_rom_sys.h>
  #include <freertos/FreeRTOS.h>
  #include <freertos/task.h>

inline void startup_rom_checkpoint(const char *stage)
{
  esp_rom_printf("\r\n[boot] %s\r\n", stage);
}

inline void startup_checkpoint(const char *stage)
{
  const UBaseType_t stack_words = uxTaskGetStackHighWaterMark(nullptr);
  Serial.printf("[boot] %s | uptime=%lu ms | stack_free=%lu B"
                " | heap_free=%lu B\n",
                stage,
                static_cast<unsigned long>(millis()),
                static_cast<unsigned long>(stack_words * sizeof(StackType_t)),
                static_cast<unsigned long>(ESP.getFreeHeap()));
  Serial.flush();
}
#else
inline void startup_rom_checkpoint(const char *)
{
}

inline void startup_checkpoint(const char *)
{
}
#endif

#endif
