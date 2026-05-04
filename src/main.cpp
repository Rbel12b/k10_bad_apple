#define AUDIO_FILENAME "/44100_u16le.pcm"
#define FPS 10
#define MJPEG_FILENAME "/320_10fps.mjpeg"
#define MJPEG_BUFFER_SIZE (320 * 240 * 8)

#define AUDIO_SAMPLE_RATE 44100
#define AUDIO_DMA_BUF_COUNT 32
#define AUDIO_DMA_BUF_LEN 1024

#include <SD.h>
#include <driver/i2s.h>
#include <Wire.h>
#include <TFT_eSPI.h>
#include "pins.h"
#include <TJpg_Decoder.h>

TFT_eSPI _tft(240, 320);

int next_frame = 0;
int skipped_frames = 0;
unsigned long total_read_audio = 0;
unsigned long total_play_audio = 0;
unsigned long total_read_video = 0;
unsigned long total_play_video = 0;
unsigned long total_remain = 0;
unsigned long start_ms, curr_ms, next_frame_ms;
int time_used = 0;

byte _xl9535P0Out, _xl9535P1Out = 0;
static const i2s_port_t I2S_PORT = I2S_NUM_0;

volatile bool running = true;
volatile bool audioStreamReady = false;
volatile bool videoStreamReady = false;
volatile bool audioStreamDone = false;
volatile bool videoStreamDone = false;

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

void audioStreamTask(void *param)
{
    File aFile = SD.open(AUDIO_FILENAME);
    if (!aFile || aFile.isDirectory())
    {
        log_d("ERROR: Failed to open " AUDIO_FILENAME " file for reading!");
        _tft.println(F("ERROR: Failed to open " AUDIO_FILENAME " file for reading!"));
        return;
    }

    uint16_t *aBuf = (uint16_t *)malloc(AUDIO_DMA_BUF_LEN); // 1 second of mono 16-bit audio
    if (!aBuf)
    {
        log_d("aBuf malloc failed!");
        return;
    }

    log_d("PCM audio start");

    audioStreamReady = true;

    while (!videoStreamReady || !running)
    {
        delay(1);
    }

    size_t bytes_written;
    while (aFile.available())
    {
        // Read audio
        size_t readBytes = aFile.read((uint8_t*)aBuf, AUDIO_DMA_BUF_LEN);
        for (size_t i = 0; i < readBytes / 2; ++i)
        {
            aBuf[i] = aBuf[i] >> 4;
        }
        // Play audio
        i2s_write(I2S_PORT, aBuf, readBytes, &bytes_written, portMAX_DELAY);
        // ignore bytes_written since we use blocking write and the buffer is large enough for one chunk of audio
    }

    aFile.close();

    audioStreamDone = true;

    log_d("PCM audio end");

    vTaskDelete(NULL);
}

#define FRAMEBUF_SIZE (320 * 240 * 4)

bool readFrame(File &f, uint8_t *buf, size_t &len)
{
    bool started = false;
    len = 0;
    static uint8_t* frameBuf;
    static size_t frameBufLen = 0;
    static size_t frameBufPos = 0;
    if (!frameBuf)
    {
        frameBuf = (uint8_t*)malloc(FRAMEBUF_SIZE);
        if (!frameBuf)
        {
            log_d("frameBuf malloc failed!");
            return false;
        }
    }

    if (frameBufLen <= 0)
    {
        frameBufLen = f.read(frameBuf, FRAMEBUF_SIZE);
        frameBufPos = 0;
    }

    while (frameBufLen > 0 || f.available())
    {
        uint8_t b = 0;
        if (frameBufLen > 0)
        {
            b = frameBuf[frameBufPos];
            frameBufLen--;
            frameBufPos++;
        }
        else
        {
            frameBufLen = f.read(frameBuf, FRAMEBUF_SIZE);
            frameBufPos = 0;
            if (frameBufLen == 0)
                break; // EOF
            b = frameBuf[frameBufPos];
            frameBufLen--;
            frameBufPos++;
        }

        if (!started)
        {
            if (b == 0xFF)
            {
                if (frameBufLen > 0)
                {
                    if (frameBuf[frameBufPos] == 0xD8)
                    {
                        started = true;
                        buf[len++] = b;
                    }
                }
                else
                {
                    int nextByte = f.peek();
                    if (nextByte == 0xD8)
                    {
                        started = true;
                        buf[len++] = b;
                    }
                }
            }
        }
        else
        {
            buf[len++] = b;

            if (len >= MJPEG_BUFFER_SIZE)
            {
                log_d("Frame too large! Increase MJPEG_BUFFER_SIZE");
                return false;
            }

            if (b == 0xD9 && buf[len - 2] == 0xFF)
                return true;
        }
    }
    return false;
}

bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap)
{
    _tft.pushImage(x, y, w, h, bitmap);
    return true;
}

void videoStreamTask(void *param)
{
    File vFile = SD.open(MJPEG_FILENAME);
    if (!vFile || vFile.isDirectory())
    {
        log_d("ERROR: Failed to open " MJPEG_FILENAME " file for reading");
        _tft.println(F("ERROR: Failed to open " MJPEG_FILENAME " file for reading"));
        return;
    }

    uint8_t *mjpeg_buf = (uint8_t *)malloc(MJPEG_BUFFER_SIZE);
    if (!mjpeg_buf)
    {
        log_d("mjpeg_buf malloc failed!");
        return;
    }
    
    TJpgDec.setCallback(tft_output);

    log_d("MJPEG video start");

    videoStreamReady = true;

    while (!audioStreamReady || !running)
    {
        delay(1);
    }

    const uint32_t frame_time = 1000 / FPS;
    uint32_t next_frame_time = millis();

    while (vFile.available() && running)
    {
        int32_t lag = millis() - next_frame_time;

        // --- If we're behind: skip frames ---
        if (lag > 500)
        {
            vTaskDelay(1);
            log_d("Lag %d ms, skipping frames...", lag);
            // How many frames we are behind
            uint32_t frames_to_skip = (lag / frame_time) + 1;

            for (uint32_t i = 0; i < frames_to_skip; i++)
            {
                size_t dummy_len = 0;

                if (!readFrame(vFile, mjpeg_buf, dummy_len))
                    break; // EOF or error

                skipped_frames++;
            }

            next_frame_time += frames_to_skip * frame_time;

            // After skipping, continue to next loop iteration
            continue;
        }

        // --- Normal frame processing ---
        size_t len = 0;

        if (!readFrame(vFile, mjpeg_buf, len))
        {
            log_d("Failed to read frame, skipping...");
            skipped_frames++;
            continue;
        }

        uint32_t start = millis();

        TJpgDec.drawJpg(0, 0, mjpeg_buf, len);

        uint32_t elapsed = millis() - start;

        next_frame_time += frame_time;

        int32_t wait = next_frame_time - millis();

        if (wait > 0)
            vTaskDelay(wait / portTICK_PERIOD_MS);
    }

    vFile.close();
    videoStreamDone = true;

    _tft.fillScreen(TFT_BLACK);

    log_d("MJPEG video end, skipped frames: %d", skipped_frames);

    vTaskDelete(NULL);
}

void playVideo()
{
    xTaskCreatePinnedToCore(videoStreamTask, "videoStreamTask", 4096, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(audioStreamTask, "audioStreamTask", 4096, NULL, 1, NULL, 0);

    running = true;
}

// SPIClass SPI1(2);

void setup()
{
    Serial.begin(115200);

    Wire.setTimeOut(100);
    Wire.begin(K10_I2C_SDA, K10_I2C_SCL, K10_I2C_FREQ);
    Wire.setTimeOut(100);
    _initXL9535();

    // Init Video
    _tft.init();
    _tft.setRotation(3);
    _tft.fillScreen(TFT_BLACK);

    _xl9535WritePin(0, true); // backlight on after display init

    // Init Audio
    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = 44100,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,
        .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_STAND_PCM_SHORT | I2S_COMM_FORMAT_STAND_I2S),
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = AUDIO_DMA_BUF_COUNT,
        .dma_buf_len = AUDIO_DMA_BUF_LEN,
        .use_apll = true,
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
        log_d("ERROR: Unable to install I2S drives!");
        _tft.println(F("ERROR: Unable to install I2S drives!"));
        return;
    }

    i2s_set_pin(I2S_PORT, &pins);
    i2s_zero_dma_buffer((i2s_port_t)0);

    SPI.begin(44, 41, 42, -1);
    // if (!SD.begin(K10_SD_CS, SPI, K10_SD_FREQ, "/sdcard"))
    if (!SD.begin())
    {
        log_d("ERROR: SD card mount failed!");
        _tft.println(F("ERROR: SD card mount failed!"));
        return;
    }

    playVideo();
}

void loop()
{
    vTaskDelay(pdMS_TO_TICKS(10));
    while (Serial.available()) {
        if (Serial.read() == 0x03)
            ESP.restart();
    }
}