#pragma once
// Copy this file to "secrets.h" (same folder) and fill it in.
// "secrets.h" is git-ignored so your Wi-Fi password never reaches GitHub.

#define WIFI_SSID "your-wifi-name"
#define WIFI_PASS "your-wifi-password"

// Shared secret for the control API. Must match ESP32_API_KEY in the server's .env file.
// Leave empty ("") to disable the check (not recommended on a shared network).
#define API_KEY "change-me"
