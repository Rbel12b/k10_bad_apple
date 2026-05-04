#define AUDIO_FILENAME "/44100_u16le.pcm"
#define FPS 30
#define MJPEG_FILENAME "/220_30fps.mjpeg"
#define MJPEG_BUFFER_SIZE (220 * 176 * 2 / 4)
// #define FPS 15
// #define MJPEG_FILENAME "/320_15fps.mjpeg"
// #define MJPEG_BUFFER_SIZE (320 * 240 * 2 / 4)
#define READ_BUFFER_SIZE 2048
#include <WiFi.h>
#include <SD.h>
#include <driver/i2s.h>
#include <Wire.h>
#include <TFT_eSPI.h>
#include "pins.h"

TFT_eSPI _tft;

#include "MjpegClass.h"
static MjpegClass mjpeg;

int next_frame = 0;
int skipped_frames = 0;
unsigned long total_read_audio = 0;
unsigned long total_play_audio = 0;
unsigned long total_read_video = 0;
unsigned long total_play_video = 0;
unsigned long total_remain = 0;
unsigned long start_ms, curr_ms, next_frame_ms;

byte _xl9535P0Out, _xl9535P1Out = 0;
static const i2s_port_t I2S_PORT = I2S_NUM_0;

void _initXL9535()
{
    uint8_t err;
    // Write each register individually (no auto-increment assumed)
    // Config port 0: bits 0,1 = output (LCD_BL, CAM_RST); bits 2-7 = input
    Wire.beginTransmission(K10_XL9535_ADDR);
    Wire.write(0x06);
    Wire.write(0xFC);
    err = Wire.endTransmission();
    if (err)
        log_e("XL9535 cfg0 err=%d", err);

    // Config port 1: bit 7 = output (AMP_GAIN); bits 0-6 = input
    Wire.beginTransmission(K10_XL9535_ADDR);
    Wire.write(0x07);
    Wire.write(0x7F);
    err = Wire.endTransmission();
    if (err)
        log_e("XL9535 cfg1 err=%d", err);

    // Output port 0: all outputs low
    _xl9535P0Out = 0x00;
    Wire.beginTransmission(K10_XL9535_ADDR);
    Wire.write(0x02);
    Wire.write(_xl9535P0Out);
    err = Wire.endTransmission();
    if (err)
        log_e("XL9535 out0 err=%d", err);

    // Output port 1: all outputs low
    _xl9535P1Out = 0x00;
    Wire.beginTransmission(K10_XL9535_ADDR);
    Wire.write(0x03);
    Wire.write(_xl9535P1Out);
    err = Wire.endTransmission();
    if (err)
        log_e("XL9535 out1 err=%d", err);
}

void _xl9535Flush(uint8_t port)
{
    // caller must hold _wireMtx
    uint8_t reg = (port == 0) ? 0x02 : 0x03;
    uint8_t val = (port == 0) ? _xl9535P0Out : _xl9535P1Out;
    Wire.beginTransmission(K10_XL9535_ADDR);
    Wire.write(reg);
    Wire.write(val);
    uint8_t err = Wire.endTransmission();
    if (err)
        log_e("XL9535 flush port%d err=%d", port, err);
}

void _xl9535WritePin(uint8_t pin, bool val)
{
    uint8_t port = pin / 8;
    uint8_t bit = pin % 8;
    if (port == 0)
    {
        if (val)
            _xl9535P0Out |= (1 << bit);
        else
            _xl9535P0Out &= ~(1 << bit);
    }
    else
    {
        if (val)
            _xl9535P1Out |= (1 << bit);
        else
            _xl9535P1Out &= ~(1 << bit);
    }
    _xl9535Flush(port);
}

SPIClass SPI1(HSPI);

void setup()
{
    WiFi.mode(WIFI_OFF);
    Serial.begin(115200);

    Wire.setTimeOut(100);
    Wire.begin(K10_I2C_SDA, K10_I2C_SCL, K10_I2C_FREQ);
    Wire.setTimeOut(100);
    _initXL9535();

    SPI1.begin(K10_LCD_SCK, K10_LCD_MISO, K10_LCD_MOSI, -1);

    // Init Video
    _tft.init();
    _tft.setRotation(2);
    _tft.fillScreen(TFT_BLACK);

    _xl9535WritePin(0, true); // backlight on after display init

    // Init Audio
    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = 44100,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_STAND_PCM_SHORT | I2S_COMM_FORMAT_STAND_I2S),
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 490,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0,
    };
    i2s_pin_config_t pins = {
        .mck_io_num = K10_I2S_MCLK,
        .bck_io_num = K10_I2S_BLCK,
        .ws_io_num = K10_I2S_LRCK,
        .data_out_num = K10_I2S_DOUT,
        .data_in_num = K10_I2S_DSIN,
    };

    if (i2s_driver_install(I2S_PORT, &cfg, 0, nullptr) != ESP_OK)
    {
        Serial.println(F("ERROR: Unable to install I2S drives!"));
        _tft.println(F("ERROR: Unable to install I2S drives!"));
        return;
    }

    i2s_set_pin(I2S_PORT, &pins);
    i2s_zero_dma_buffer((i2s_port_t)0);

    // Init SD card
    if (!SD.begin(K10_SD_CS, SPI1, K10_LCD_FREQ, "/sdcard"))
    {
        Serial.println(F("ERROR: SD card mount failed!"));
        _tft.println(F("ERROR: SD card mount failed!"));
        return;
    }

    File aFile = SD.open(AUDIO_FILENAME);
    if (!aFile || aFile.isDirectory())
    {
        Serial.println(F("ERROR: Failed to open " AUDIO_FILENAME " file for reading!"));
        _tft.println(F("ERROR: Failed to open " AUDIO_FILENAME " file for reading!"));
        return;
    }

    File vFile = SD.open(MJPEG_FILENAME);
    if (!vFile || vFile.isDirectory())
    {
        Serial.println(F("ERROR: Failed to open " MJPEG_FILENAME " file for reading"));
        _tft.println(F("ERROR: Failed to open " MJPEG_FILENAME " file for reading"));
        return;
    }

    uint8_t *aBuf = (uint8_t *)malloc(2940);
    // uint8_t *aBuf = (uint8_t *)malloc(5880);
    if (!aBuf)
    {
        Serial.println(F("aBuf malloc failed!"));
        return;
    }

    uint8_t *mjpeg_buf = (uint8_t *)malloc(MJPEG_BUFFER_SIZE);
    if (!mjpeg_buf)
    {
        Serial.println(F("mjpeg_buf malloc failed!"));
        return;
    }

    Serial.println(F("PCM audio MJPEG video start"));
    start_ms = millis();
    curr_ms = millis();
    mjpeg.setup(vFile, mjpeg_buf, &_tft, true);
    next_frame_ms = start_ms + (++next_frame * 1000 / FPS);

    // prefetch first audio buffer
    size_t bytes_written;
    aFile.read(aBuf, 2940);
    i2s_write(I2S_PORT, aBuf, 2940, &bytes_written, portMAX_DELAY);

    while (vFile.available() && aFile.available())
    {
        // Read audio
        curr_ms = millis();
        aFile.read(aBuf, 2940);
        // aFile.read(aBuf, 5880);
        total_read_audio += millis() - curr_ms;
        curr_ms = millis();

        // Play audio
        i2s_write(I2S_PORT, aBuf, 2940, &bytes_written, portMAX_DELAY);
        // // for 15 FPS
        // i2s_write(I2S_PORT, aBuf, 5880, &bytes_written, portMAX_DELAY);
        total_play_audio += millis() - curr_ms;
        curr_ms = millis();

        // Read video
        mjpeg.readMjpegBuf();
        total_read_video += millis() - curr_ms;
        curr_ms = millis();

        if (millis() < next_frame_ms) // check show frame or skip frame
        {
            // Play video
            mjpeg.drawJpg();
            total_play_video += millis() - curr_ms;

            int remain_ms = next_frame_ms - millis();
            total_remain += remain_ms;
            if (remain_ms > 0)
            {
                delay(remain_ms);
            }
        }
        else
        {
            ++skipped_frames;
            Serial.println(F("Skip frame"));
        }

        curr_ms = millis();
        next_frame_ms = start_ms + (++next_frame * 1000 / FPS);
    }
    int time_used = millis() - start_ms;
    Serial.println(F("PCM audio MJPEG video end"));
    vFile.close();
    aFile.close();
    int played_frames = next_frame - 1 - skipped_frames;
    float fps = 1000.0 * played_frames / time_used;
    Serial.printf("Played frames: %d\n", played_frames);
    Serial.printf("Skipped frames: %d (%0.1f %%)\n", skipped_frames, 100.0 * skipped_frames / played_frames);
    Serial.printf("Time used: %d ms\n", time_used);
    Serial.printf("Expected FPS: %d\n", FPS);
    Serial.printf("Actual FPS: %0.1f\n", fps);
    Serial.printf("SDMMC read PCM: %d ms (%0.1f %%)\n", total_read_audio, 100.0 * total_read_audio / time_used);
    Serial.printf("Play audio: %d ms (%0.1f %%)\n", total_play_audio, 100.0 * total_play_audio / time_used);
    Serial.printf("SDMMC read MJPEG: %d ms (%0.1f %%)\n", total_read_video, 100.0 * total_read_video / time_used);
    Serial.printf("Play video: %d ms (%0.1f %%)\n", total_play_video, 100.0 * total_play_video / time_used);
    Serial.printf("Remain: %d ms (%0.1f %%)\n", total_remain, 100.0 * total_remain / time_used);

#define CHART_MARGIN 24
#define LEGEND_A_COLOR 0xE0C3
#define LEGEND_B_COLOR 0x33F7
#define LEGEND_C_COLOR 0x4D69
#define LEGEND_D_COLOR 0x9A74
#define LEGEND_E_COLOR 0xFBE0
#define LEGEND_F_COLOR 0xFFE6
#define LEGEND_G_COLOR 0xA2A5
    _tft.setCursor(0, 0);
    _tft.setTextColor(TFT_WHITE);
    _tft.printf("Played frames: %d\n", played_frames);
    _tft.printf("Skipped frames: %d (%0.1f %%)\n", skipped_frames, 100.0 * skipped_frames / played_frames);
    _tft.printf("Actual FPS: %0.1f\n\n", fps);
    int16_t r1 = ((_tft.height() - CHART_MARGIN - CHART_MARGIN) / 2);
    int16_t r2 = r1 / 2;
    int16_t cx = _tft.width() - _tft.height() + CHART_MARGIN + CHART_MARGIN - 1 + r1;
    int16_t cy = r1 + CHART_MARGIN;
    float arc_start = 0;
    float arc_end = max(2.0, 360.0 * total_read_audio / time_used);
    for (int i = arc_start + 1; i < arc_end; i += 2)
    {
        _tft.drawArc(cx, cy, r1, r2, arc_start - 90.0, i - 90.0, LEGEND_D_COLOR, LEGEND_D_COLOR);
    }
    _tft.drawArc(cx, cy, r1, r2, arc_start - 90.0, arc_end - 90.0, LEGEND_D_COLOR, LEGEND_D_COLOR);
    _tft.setTextColor(LEGEND_D_COLOR);
    _tft.printf("Read PCM:\n%0.1f %%\n", 100.0 * total_read_audio / time_used);

    arc_start = arc_end;
    arc_end += max(2.0, 360.0 * total_play_audio / time_used);
    for (int i = arc_start + 1; i < arc_end; i += 2)
    {
        _tft.drawArc(cx, cy, r1, r2, arc_start - 90.0, i - 90.0, LEGEND_C_COLOR, LEGEND_C_COLOR);
    }
    _tft.drawArc(cx, cy, r1, r2, arc_start - 90.0, arc_end - 90.0, LEGEND_C_COLOR, LEGEND_C_COLOR);
    _tft.setTextColor(LEGEND_C_COLOR);
    _tft.printf("Play audio:\n%0.1f %%\n", 100.0 * total_play_audio / time_used);

    arc_start = arc_end;
    arc_end += max(2.0, 360.0 * total_read_video / time_used);
    for (int i = arc_start + 1; i < arc_end; i += 2)
    {
        _tft.drawArc(cx, cy, r1, r2, arc_start - 90.0, i - 90.0, LEGEND_B_COLOR, LEGEND_B_COLOR);
    }
    _tft.drawArc(cx, cy, r1, r2, arc_start - 90.0, arc_end - 90.0, LEGEND_B_COLOR, LEGEND_B_COLOR);
    _tft.setTextColor(LEGEND_B_COLOR);
    _tft.printf("Read MJPEG:\n%0.1f %%\n", 100.0 * total_read_video / time_used);

    arc_start = arc_end;
    arc_end += max(2.0, 360.0 * total_play_video / time_used);
    for (int i = arc_start + 1; i < arc_end; i += 2)
    {
        _tft.drawArc(cx, cy, r1, 0, arc_start - 90.0, i - 90.0, LEGEND_A_COLOR, LEGEND_A_COLOR);
    }
    _tft.drawArc(cx, cy, r1, 0, arc_start - 90.0, arc_end - 90.0, LEGEND_A_COLOR, LEGEND_A_COLOR);
    _tft.setTextColor(LEGEND_A_COLOR);
    _tft.printf("Play video:\n%0.1f %%\n", 100.0 * total_play_video / time_used);

    i2s_driver_uninstall((i2s_port_t)0); // stop & destroy i2s driver
    // avoid unexpected output at audio pins
    pinMode(25, OUTPUT);
    digitalWrite(25, LOW);
    pinMode(26, OUTPUT);
    digitalWrite(26, LOW);
    // _tft.displayOff();
    // esp_deep_sleep_start();
}

void loop()
{
    vTaskDelay(pdMS_TO_TICKS(10));
    while (Serial.available()) {
        if (Serial.read() == 0x03)
            ESP.restart();
    }
}