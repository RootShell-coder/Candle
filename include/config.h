#pragma once

#include <Arduino.h>

#define CONFIG_JSON_PATH "/settings.json"

struct WiFiConfig {
  char devname[32];
  char name[64];
  char ssid[64];
  char password[64];
  int power;
  char phy_mode[8];
};

struct OtaConfig {
  int port;
  char hostname[64];
};

struct NtpConfig {
  char ntp_server[64];
  char ntp_timezone[32];
};

struct LocationConfig {
  bool enabled;
  double lat;
  double lng;
};

struct Config {
  WiFiConfig wifi;
  OtaConfig ota;
  NtpConfig ntp;
  LocationConfig location;
  uint8_t brightness;
  bool candleOn;
};

bool configLoad();
bool configSave();
bool configSetBrightness(uint8_t brightness);
const Config& getConfig();
Config& getMutableConfig();
void configResetToDefault();
