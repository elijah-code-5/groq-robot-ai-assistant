#pragma once

// WiFi
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"

// Bluetooth speaker name
#define BT_SPEAKER "JBL Go 4"

// Groq settings
#define GROQ_API_KEY "gsk_your_key_here"
#define GROQ_MODEL "llama-3.3-70b-versatile"
#define ENABLE_GROQ_TTS 0

// Optional TTS settings if you later enable the existing Wit path
#define ENABLE_TTS 0
#define WIT_TOKEN ""
#define WIT_VOICE "Rebecca"

// Robot behavior
#define REQUIRE_LOGIN 0
#define WEB_USER "admin"
#define WEB_PASS "change-me"
#define GMT_OFFSET_S 18000
#define DRIVE_SPEED 2000
#define DRIVE_MS 2000
#define ALARM_MAX_MS (15UL * 60UL * 1000UL)
#define BT_PREWARM_MIN 2

// Default alarm/podcast values
#define DEFAULT_PODCAST_NAME "Podcast 1"
#define DEFAULT_PODCAST_URL "https://dcs-cached.megaphone.fm/ARML4940363433.mp3"
