# Groq Robot Alarm Clock + AI Assistant

ESP32 robot assistant for a Freenove 4WD car kit with:

- Groq AI voice/chat assistant
- podcast player
- Bluetooth speaker support
- alarm clock and timer
- web dashboard
- robot movement controls
- local flash storage for settings

This project keeps the original robot alarm system behavior and extends it with Groq-powered conversation, voice commands, and assistant features.

## Features

- ESP32 Wi-Fi connection
- Bluetooth speaker audio playback
- MP3 podcast streaming over HTTP
- Freenove 4WD car motor control
- daily / weekday / once-only alarms
- timer function
- web UI for alarms, podcasts, and controls
- voice command processing
- Groq AI assistant for general requests
- optional Groq TTS output
- flash-based podcast and alarm persistence

## Hardware

- ESP32-WROOM-32
- Freenove 4WD Smart Car Kit
- PCA9685 motor driver board
- Bluetooth speaker (example: JBL Go 4)
- 5V power supply for motors and controller

## Required software

For Arch Linux, install:

```bash
sudo pacman -Syu --noconfirm
sudo pacman -S --noconfirm \
  base-devel git wget curl python python-pip python-pyserial \
  usbutils libusb platformio

sudo groupadd dialout
sudo usermod -aG dialout $USER
sudo usermod -aG uucp $USER
```

Then log out and log back in, or use:

```bash
su - $USER
```

Install the ESP32 serial rules:

```bash
sudo tee /etc/udev/rules.d/99-esp32.rules >/dev/null <<'EOF'
# CH340
SUBSYSTEM=="tty", ATTRS{idVendor}=="1a86", ATTRS{idProduct}=="7523", MODE="0666"
SUBSYSTEM=="usb", ATTRS{idVendor}=="1a86", ATTRS{idProduct}=="7523", MODE="0666"

# CP210x
SUBSYSTEM=="tty", ATTRS{idVendor}=="10c4", ATTRS{idProduct}=="ea60", MODE="0666"
SUBSYSTEM=="usb", ATTRS{idVendor}=="10c4", ATTRS{idProduct}=="ea60", MODE="0666"

# FTDI
SUBSYSTEM=="tty", ATTRS{idVendor}=="0403", ATTRS{idProduct}=="6001", MODE="0666"
SUBSYSTEM=="usb", ATTRS{idVendor}=="0403", ATTRS{idProduct}=="6001", MODE="0666"
EOF

sudo udevadm control --reload-rules
sudo udevadm trigger
```

Install Arduino libraries used by the project:

```bash
git clone https://github.com/Freenove/Freenove_4WD_Car_For_ESP32.git \
  ~/.local/share/Arduino/libraries/Freenove_4WD_Car_For_ESP32

git clone https://github.com/pschatzmann/Arduino-AudioTools.git \
  ~/.local/share/Arduino/libraries/AudioTools
```

## PlatformIO project

Create a project folder:

```bash
mkdir -p ~/freenove-robot/src
cd ~/freenove-robot
```

Create `platformio.ini`:

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
monitor_speed = 115200
upload_speed = 115200

lib_deps =
  https://github.com/pschatzmann/Arduino-AudioTools.git
```

Create `src/config.h`:

```cpp
#pragma once

#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"

#define BT_SPEAKER "JBL Go 4"
#define GMT_OFFSET_S 18000

#define GROQ_API_KEY "gsk_YOUR_KEY_HERE"
#define GROQ_MODEL "llama-3.3-70b-versatile"

#define ENABLE_GROQ_TTS 1
#define ENABLE_TTS 0
#define WIT_TOKEN ""
#define WIT_VOICE "Rebecca"

#define REQUIRE_LOGIN 0
#define WEB_USER "admin"
#define WEB_PASS "change-me"

#define DRIVE_SPEED 2000
#define DRIVE_MS 2000
#define ALARM_MAX_MS (15UL * 60UL * 1000UL)
#define BT_PREWARM_MIN 2

#define DEFAULT_PODCAST_NAME "Podcast 1"
#define DEFAULT_PODCAST_URL "https://dcs-cached.megaphone.fm/ARML4940363433.mp3"
```

## Build and upload

```bash
cd ~/freenove-robot
platformio run --target upload
platformio device monitor --baud 115200
```

If PlatformIO is not found, install it first:

```bash
pip install --upgrade platformio pyserial esptool --break-system-packages 2>/dev/null || pip install --upgrade platformio pyserial esptool
```

## Common commands

Serial monitor commands:

- `f` = forward
- `b` = backward
- `s` = stop
- `p` = play first podcast
- `v<text>` = send a voice command

Web interface:

- open the ESP32 IP shown in the serial monitor
- examples: alarms, podcasts, timer, voice assistant, movement controls

## Groq assistant

The robot can send natural-language requests to Groq. Example commands:

- "hey robot go forward"
- "play podcast 1"
- "what time is it"
- "stop"
- "tell me a joke"
- "what day is it"

The assistant can answer general questions or perform robot actions such as movement, podcast playback, or timer updates.

## Project status

This repository is intended for a Freenove 4WD robot built around an ESP32 and a Bluetooth speaker. It combines:

- robot driving
- alarms
- podcast playback
- assistant logic
- Groq API integration
- web UI control

## Notes

- Use a 2.4GHz Wi-Fi network with the ESP32.
- Make sure the speaker name matches your actual Bluetooth speaker.
- Keep your Groq API key private and never commit it to a public repository.
- If the serial port does not appear, check `ls /dev/ttyUSB*` and reload udev rules.

## Troubleshooting

If the board is not detected:

```bash
ls /dev/ttyUSB*
ls /dev/ttyACM*
```

If nothing appears, inspect your USB cable and confirm the ESP32 board is connected properly.

If the Groq API fails, confirm:

- Wi-Fi is connected
- the `GROQ_API_KEY` is valid
- the model name matches a valid Groq model
- your Internet connection is working

## License

This project is provided as-is for educational and personal robotics use.
