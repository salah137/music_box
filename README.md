# Musicano

ESP32-based Bluetooth MP3 player. Decodes MP3s from SD card and streams audio over Classic Bluetooth A2DP. Wi-Fi soft-AP for wireless file uploads. SSD1306 OLED UI with 4 buttons.

## Hardware

- ESP32 (use ESP32-S3 for stable full-feature operation)
- SSD1306 OLED via I2C (SDA=21, SCL=22)
- MicroSD via SPI (MISO=19, MOSI=23, CLK=18, CS=25)
- 4x buttons (TOP_RIGHT=14, TOP_LEFT=27, BOTTOM_RIGHT=13, BOTTOM_LEFT=12)

## How it works

`sdcard_task` decodes MP3 frames with minimp3 and pushes PCM into a FreeRTOS ring buffer. The A2DP audio callback drains that buffer and streams to whatever Bluetooth speaker is connected.

Wi-Fi runs a soft-AP (`esp_wifi` / `esp230005`) with an HTTP upload server at `192.168.4.1` so you can drop MP3s onto the SD card from a browser.

## Build

```bash
idf.py set-target esp32s3
idf.py build && idf.py flash monitor
```

Requires ESP-IDF v6.x and [minimp3](https://github.com/lieff/minimp3).

## Note

Full feature set (BT + WiFi + MP3) exceeds available RAM on ESP32 WROOM. Use ESP32-S3.