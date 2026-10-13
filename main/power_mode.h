#pragma once

#include <stdbool.h>

/* Volatile mode: every reset starts in normal mode. Network transitions are
 * owned by network_monitor_task, never by the GPIO/timer callbacks. */
void power_mode_toggle(void);
void power_mode_set_low(bool low);
bool power_mode_is_low(void);
bool power_mode_weather_network_requested(void);
