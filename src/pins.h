#pragma once

// ── Display (ILI9341, SPI shared with SD) ────────────────────────────────────
#define K10_LCD_MOSI      21
#define K10_LCD_SCK       12
#define K10_LCD_CS        14
#define K10_LCD_DC        13
#define K10_LCD_MISO      41
#define K10_LCD_RST       -1
#define K10_LCD_BL        -1  // controlled via XL9535 (K10_XL9535_LCD_BL), not direct GPIO
#define K10_LCD_FREQ      40000000UL
#define K10_LCD_WIDTH     320
#define K10_LCD_HEIGHT    240

// ── SD Card (shares SPI bus with LCD) ────────────────────────────────────────
#define K10_SD_CS         40
#define K10_SD_FREQ       20000000UL

// ── Camera (OV2640, 8-bit parallel) ──────────────────────────────────────────
#define K10_CAM_XCLK      7
#define K10_CAM_SIOD      47
#define K10_CAM_SIOC      48
#define K10_CAM_D0        8
#define K10_CAM_D1        10
#define K10_CAM_D2        11
#define K10_CAM_D3        9
#define K10_CAM_D4        18
#define K10_CAM_D5        16
#define K10_CAM_D6        15
#define K10_CAM_D7        6
#define K10_CAM_VSYNC     4
#define K10_CAM_HREF      5
#define K10_CAM_PCLK      17
#define K10_CAM_PWDN      -1
#define K10_CAM_RESET     -1  // controlled via XL9535 (K10_XL9535_CAM_RST), not direct GPIO
#define K10_CAM_XCLK_FREQ 8000000UL

// ── I2S (audio) ───────────────────────────────────────────────────────────────
#define K10_I2S_BLCK      0
#define K10_I2S_LRCK      38
#define K10_I2S_DSIN      39  // microphone in
#define K10_I2S_DOUT      45  // speaker out
#define K10_I2S_MCLK      3

// ── I2C bus ───────────────────────────────────────────────────────────────────
#define K10_I2C_SDA       47
#define K10_I2C_SCL       48
#define K10_I2C_FREQ      400000UL

// ── RGB LEDs (WS2812B) ────────────────────────────────────────────────────────
#define K10_RGB_PIN       46
#define K10_RGB_COUNT     3

// ── I2C device addresses ──────────────────────────────────────────────────────
#define K10_IMU_ADDR      0x19  // SC7A20H accelerometer
#define K10_AHT20_ADDR    0x38  // AHT20 temperature/humidity
#define K10_ALS_ADDR      0x29  // LTR303ALS ambient light sensor
#define K10_XL9535_ADDR   0x20  // XL9535 I/O expander (TCA9535-compatible, A0=A1=A2=GND)

// ── XL9535 logical pin numbers (0-15: port0[0..7] + port1[0..7]) ─────────────
// Output pins (configured as outputs in _initXL9535):
#define K10_XL9535_LCD_BL   0   // port0 bit0 — LCD backlight (HIGH = on)
#define K10_XL9535_CAM_RST  1   // port0 bit1 — camera reset (LOW = reset)
#define K10_XL9535_AMP_GAIN 15  // port1 bit7 — amp gain enable
// Input pins (active LOW, pulled up internally):
#define K10_XL9535_BTN_B    2   // port0 bit2 — button B
#define K10_XL9535_BTN_A    12  // port1 bit4 — button A
