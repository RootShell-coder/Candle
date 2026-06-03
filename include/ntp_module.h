#pragma once

void ntp_init();
void ntp_handle();
bool ntp_has_valid_time();

// Возвращает текущее смещение локального времени от UTC в минутах.
// Учитывает DST на основе таймзоны, применённой NTP-модулем.
int ntp_utc_offset_minutes();
