#pragma once

#include <Arduino.h>

#define CONFIG_JSON_PATH "/settings.json"

struct NtpConfig {
  char ntp_server[64];
  char ntp_server2[64];
  char ntp_timezone[32];
};

struct LocationConfig {
  bool enabled;
  double lat;
  double lng;
};

struct MoonLedConfig {
  bool enabled;
  uint8_t maxBrightness;
  uint16_t hue;
};

struct TimeScheduleConfig {
  bool enabled;
  uint16_t onMinute;
  uint16_t offMinute;
};

struct Config {
  char devname[32];
  char name[64];
  NtpConfig ntp;
  LocationConfig location;
  MoonLedConfig moonLed;
  TimeScheduleConfig timeSchedule;
  uint8_t brightness;
  bool candleOn;
};

bool configLoad();
bool configSave();
bool configUpdate(const Config& config, bool saveToFile = true);
Config getConfig();
