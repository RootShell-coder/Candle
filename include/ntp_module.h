#pragma once

#include <stdint.h>

void ntp_init();
void ntp_handle();
bool ntp_has_valid_time();
bool ntp_is_synchronized();
bool ntp_set_manual_time(uint32_t epochUtc);

struct NtpStatus {
  bool wifiConnected;
  bool hasIp;
  bool syncInProgress;
  bool validTime;
  bool ntpSynchronized;
  bool bootSyncPending;
  bool dnsResolved;
  uint8_t failureStreak;
  uint8_t sntpSyncStatus;
  uint32_t nextSyncInMs;
  uint32_t syncTimeoutInMs;
  uint32_t lastSuccessEpoch;
  char server[64];
  char serverIp[46];
  char timezone[32];
};

NtpStatus ntp_status();

int ntp_utc_offset_minutes();
