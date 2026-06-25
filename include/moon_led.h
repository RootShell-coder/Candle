#pragma once

#include <stdint.h>

void moon_led_init();
void moon_led_apply();
void moon_led_set_matrix_active(bool active);
uint8_t moon_led_current_brightness();
bool moon_led_hardware_enabled();
