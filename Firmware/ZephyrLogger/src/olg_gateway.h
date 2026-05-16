#pragma once

#include <stdbool.h>
#include <stdint.h>

int olg_gateway_init(void);
void olg_gateway_service(uint32_t now_ms);
uint32_t olg_gateway_ms_until_transition(uint32_t now_ms);
bool olg_gateway_radio_active(void);
