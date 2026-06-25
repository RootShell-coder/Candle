#pragma once

#include <Arduino.h>

void smooth_change_init(uint8_t initialValue = 0);
void smooth_change_set_target(uint8_t targetValue);
uint8_t smooth_change_update();
bool smooth_change_is_active();
