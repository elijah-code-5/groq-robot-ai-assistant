# Groq Robot Alarm Clock

This project turns your ESP32 robot into a smart alarm clock and assistant that can:

- keep the podcast player and alarm clock behavior
- connect to Wi‑Fi and a Bluetooth speaker
- drive with web controls or voice commands
- use the Groq API for assistant reasoning
- optionally speak Groq answers through the speaker
- save podcasts and alarms in flash memory

## Features

- 4WD robot motion using the Freenove 4WD car library
- custom web dashboard for alarms, podcasts, timer, and voice control
- podcast URLs stored in ESP32 flash
- daily/weekday/once alarm scheduling
- assistant mode that understands natural language via Groq
- optional Groq text-to-speech support

## Project structure

- `src/config.h` — Wi‑Fi, Groq, speaker, and behavior settings
- `src/RobotAlarmClock_Groq.ino` — main Arduino sketch
- `platformio.ini` — optional PlatformIO configuration for ESP32

## Setup

1. Fill in your Wi‑Fi settings and Groq API key in `src/config.h`.
2. Adjust `BT_SPEAKER`, `WIFI_SSID`, `WIFI_PASS`, and your alarm defaults.
3. Open the sketch in Arduino IDE or PlatformIO.
4. Install the required libraries:
   - `AudioTools`
   - `ESP32-A2DP`
   - `Freenove_4WD_Car_For_ESP32`
5. Upload to the ESP32.
6. Open the web interface at the IP shown in the serial monitor.

## Groq assistant behavior

The assistant is triggered when the user says commands outside the built-in robot actions. The sketch sends text to Groq using the chat completions API and then:

- logs the response
- optionally reads it aloud using Groq TTS or the existing TTS pipeline
- keeps the robot controls and alarms active at the same time

## Notes

- The Groq API key should never be committed to a public repository.
- For production, use your own credentials and keep `config.h` private.
- This project is built from your original ESP32 robot alarm clock and extended with Groq-based AI assistance.
