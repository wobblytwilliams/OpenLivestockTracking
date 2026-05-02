#pragma once

#include <stdint.h>

int olg_power_init(void);
void olg_power_idle(uint32_t ms);
void olg_power_pulse_status_led(uint32_t ms);
void olg_power_status_ok(void);
void olg_power_status_fault(void);
