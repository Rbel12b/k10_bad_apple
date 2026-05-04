# Bad Apple on UNIHIKER K10

Plays the Bad Apple!! video with synchronized audio on a [DFRobot UNIHIKER K10](https://www.dfrobot.com/product-2830.html) (ESP32-S3, ILI9341 320×240 display, I2S speaker).

## How it works

- Video: MJPEG file streamed from SD card, decoded frame-by-frame with TJpg_Decoder, pushed to ILI9341 via TFT_eSPI
- Audio: raw PCM (44100 Hz, 16-bit mono) streamed from SD card, output via I2S
- Two FreeRTOS tasks (pinned to separate cores) sync at startup then run in parallel
- Frame timing enforces target FPS; catches up by skipping frames if lag exceeds 500 ms

## Hardware

| Component | Details |
| --- | --- |
| Board | UNIHIKER K10 (ESP32-S3, 16MB flash, PSRAM) |
| Display | ILI9341, 320×240, SPI @ 40 MHz |
| Audio | I2S speaker (mono, 44100 Hz) |
| Storage | SD card (separate SPI bus @ 20 MHz) |
| I/O expander | XL9535 (I2C) — controls LCD backlight and amp gain |

## Video files

Place these on the root of the SD card:

| File | Description |
| --- | --- |
| `320_10fps.mjpeg` | MJPEG video, 320×240, 10 fps |
| `44100_u16le.pcm` | Raw PCM audio, 44100 Hz, 16-bit unsigned LE, mono |

The source video is included at `video/bad_apple.webm`. Convert with ffmpeg:

```sh
# Video (10 fps example)
ffmpeg -i bad_apple.webm -vf "fps=10,scale=320:240:flags=lanczos" -q:v 9 -pix_fmt yuvj420p 320_10fps.mjpeg

# Audio
ffmpeg -i bad_apple.webm -f u16le -acodec pcm_u16le -ar 44100 -ac 1 44100_u16le.pcm
```

## Configuration

Edit the top of [src/main.cpp](src/main.cpp) to change FPS or file names:

```cpp
#define FPS             10
#define MJPEG_FILENAME  "/320_10fps.mjpeg"
#define AUDIO_FILENAME  "/44100_u16le.pcm"
```

## Build & flash

Requires [PlatformIO](https://platformio.org/).

```sh
pio run -t upload
pio device monitor
```

The custom board definition (`boards/unihiker_k10.json`) and partition table (`partitions/k10_16mb.csv`) are included — no extra setup needed.

## Pin reference

See [src/pins.h](src/pins.h) for all GPIO and I2C address definitions.

## Dependencies

- [bodmer/TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) — display driver
- [bodmer/TJpg_Decoder](https://github.com/Bodmer/TJpg_Decoder) — JPEG decoder
