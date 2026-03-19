#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <driver/i2s.h>
#include <math.h>

#include "USB.h"
#include "tusb.h"
#include <NeoPixelBus.h>
#include <arduinoFFT.h>

// --- Configuration ---
static const uint32_t AUDIO_SAMPLE_RATE    = 48000;
static const uint8_t  AUDIO_CHANNELS       = 2;
static const uint16_t AUDIO_SAMPLE_COUNT   = 48; // 1ms @ 48kHz

static const uint16_t FFT_SAMPLES          = 512;
static const float    FFT_NOISE_FLOOR      = 50.0f;
static const float    FFT_SMOOTHING        = 0.7f;

static const i2s_port_t I2S_DAC_PORT       = I2S_NUM_0;
static const int PIN_I2S_BCK               = 9;
static const int PIN_I2S_LCK               = 11;
static const int PIN_I2S_DOUT              = 16;

static const uint8_t  MATRIX_WIDTH         = 16;
static const uint8_t  MATRIX_HEIGHT        = 16;
static const uint16_t MATRIX_NUM_LEDS      = 256;
static const int      PIN_LED_DATA         = 17;
static const uint8_t  MATRIX_BRIGHTNESS    = 40; // ~15% limit for safety

static const int      PIN_POT              = 21;
static const float    GAIN_MIN             = 0.1f;
static const float    GAIN_MAX             = 5.0f;

static const uint8_t  NUM_BARS             = 16;
static const float    PEAK_FALL_RATE       = 0.15f;
static const uint8_t  PEAK_HOLD_FRAMES     = 12;
static const float    BAR_DECAY_RATE       = 0.85f;

struct DisplayFrame {
    float barHeights[NUM_BARS];
    float peakPositions[NUM_BARS];
    float gain;
};

// --- Globals ---
NeoPixelBus<NeoGrbwFeature, NeoEsp32RmtNSk6812Method> strip(MATRIX_NUM_LEDS, PIN_LED_DATA);

static float fftInput[FFT_SAMPLES];
static float fftOutput[FFT_SAMPLES];
ArduinoFFT<float> FFT(fftInput, fftOutput, FFT_SAMPLES, AUDIO_SAMPLE_RATE);

static QueueHandle_t displayQueue = NULL;
static TaskHandle_t  audioTaskHandle = NULL;
static TaskHandle_t  ledTaskHandle = NULL;

static int16_t audioRingBuffer[FFT_SAMPLES];
static volatile uint16_t ringWritePos = 0;

static float smoothedBars[NUM_BARS] = {0};
static float peakValues[NUM_BARS] = {0};
static uint8_t peakHoldCounters[NUM_BARS] = {0};

static volatile bool usbAudioActive = false;

// --- USB Audio Descriptors ---
enum { ITF_NUM_AUDIO_CONTROL = 0, ITF_NUM_AUDIO_STREAMING, ITF_NUM_TOTAL };

#define TUD_AUDIO_SPEAKER_DESC_LEN (TUD_CONFIG_DESC_LEN + TUD_AUDIO_DESC_IAD_LEN + TUD_AUDIO_DESC_STD_AC_LEN + TUD_AUDIO_DESC_CS_AC_LEN + TUD_AUDIO_DESC_INPUT_TERM_LEN + TUD_AUDIO_DESC_FEATURE_UNIT_TWO_CHANNEL_LEN + TUD_AUDIO_DESC_OUTPUT_TERM_LEN + TUD_AUDIO_DESC_STD_AS_INT_LEN + TUD_AUDIO_DESC_STD_AS_INT_LEN + TUD_AUDIO_DESC_CS_AS_INT_LEN + TUD_AUDIO_DESC_TYPE_I_FORMAT_LEN + TUD_AUDIO_DESC_STD_AS_ISO_EP_LEN + TUD_AUDIO_DESC_CS_AS_ISO_EP_LEN)
#define UAC_ENTITY_SPK_INPUT_TERMINAL 0x01
#define UAC_ENTITY_SPK_FEATURE_UNIT   0x02
#define UAC_ENTITY_SPK_OUTPUT_TERMINAL 0x03
#define EPNUM_AUDIO_OUT 0x01

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUD_AUDIO_SPEAKER_DESC_LEN, 0x00, 100),
    TUD_AUDIO_DESC_IAD(ITF_NUM_AUDIO_CONTROL, ITF_NUM_TOTAL, 0),
    TUD_AUDIO_DESC_STD_AC(ITF_NUM_AUDIO_CONTROL, 0, 0),
    TUD_AUDIO_DESC_CS_AC(0x0100, TUD_AUDIO_DESC_INPUT_TERM_LEN + TUD_AUDIO_DESC_FEATURE_UNIT_TWO_CHANNEL_LEN + TUD_AUDIO_DESC_OUTPUT_TERM_LEN, 1, ITF_NUM_AUDIO_STREAMING),
    TUD_AUDIO_DESC_INPUT_TERM(UAC_ENTITY_SPK_INPUT_TERMINAL, AUDIO_TERM_TYPE_USB_STREAMING, 0x00, AUDIO_CHANNELS, 0x0003, 0),
    TUD_AUDIO_DESC_FEATURE_UNIT_TWO_CHANNEL(UAC_ENTITY_SPK_FEATURE_UNIT, UAC_ENTITY_SPK_INPUT_TERMINAL, 0x0001, 0x0002, 0x0002, 0),
    TUD_AUDIO_DESC_OUTPUT_TERM(UAC_ENTITY_SPK_OUTPUT_TERMINAL, AUDIO_TERM_TYPE_OUT_GENERIC_SPEAKER, 0x00, UAC_ENTITY_SPK_FEATURE_UNIT, 0),
    TUD_AUDIO_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING, 0x00, 0x00, 0),
    TUD_AUDIO_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING, 0x01, 0x01, 0),
    TUD_AUDIO_DESC_CS_AS_INT(UAC_ENTITY_SPK_INPUT_TERMINAL, 0x01, 0x0001),
    TUD_AUDIO_DESC_TYPE_I_FORMAT(AUDIO_CHANNELS, 2, 16, 0x01, AUDIO_SAMPLE_RATE),
    TUD_AUDIO_DESC_STD_AS_ISO_EP(EPNUM_AUDIO_OUT, (0x01 | 0x10), (AUDIO_SAMPLE_COUNT * 4), 0x01),
    TUD_AUDIO_DESC_CS_AS_ISO_EP(0x01, 0x00, 0x00, 0x0000),
};

static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t), .bDescriptorType = TUSB_DESC_DEVICE, .bcdUSB = 0x0200,
    .bDeviceClass = 0xEF, .bDeviceSubClass = 0x02, .bDeviceProtocol = 0x01, .bMaxPacketSize0 = 64,
    .idVendor = 0x303A, .idProduct = 0x4002, .bcdDevice = 0x0100,
    .iManufacturer = 0x01, .iProduct = 0x02, .iSerialNumber = 0x03, .bNumConfigurations = 0x01
};

static const char* string_descriptors[] = { (const char[]){0x09, 0x04}, "ESP32-S3", "Spectrum Visualizer", "SV-2026" };

extern "C" {
    void tud_mount_cb(void) {}
    void tud_umount_cb(void) { usbAudioActive = false; }
    void tud_suspend_cb(bool r) { usbAudioActive = false; }
    void tud_resume_cb(void) {}

    bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
        if (tu_u16_low(p_request->wIndex) == ITF_NUM_AUDIO_STREAMING) 
            usbAudioActive = (tu_u16_low(p_request->wValue) == 1);
        return true;
    }

    bool tud_audio_set_itf_close_EP_cb(uint8_t rhport, tusb_control_request_t const *p_request) { return true; }

    bool tud_audio_rx_done_pre_read_cb(uint8_t rhport, uint16_t n_bytes, uint8_t f_id, uint8_t ep, uint8_t alt) {
        if (n_bytes == 0) return true;
        static int16_t buf[AUDIO_SAMPLE_COUNT * 2];
        uint16_t bytes_read = tud_audio_read(buf, n_bytes);
        
        size_t written = 0;
        i2s_write(I2S_DAC_PORT, buf, bytes_read, &written, 0);
        
        uint16_t frames = bytes_read / 4;
        for (uint16_t i = 0; i < frames; i++) {
            audioRingBuffer[ringWritePos] = (int16_t)(((int32_t)buf[i*2] + buf[i*2+1]) / 2);
            ringWritePos = (ringWritePos + 1) % FFT_SAMPLES;
        }
        return true;
    }

    uint8_t const* tud_descriptor_device_cb(void) { return (uint8_t const*)&desc_device; }
    uint8_t const* tud_descriptor_configuration_cb(uint8_t index) { return desc_configuration; }
    uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
        static uint16_t _desc_str[33];
        if (index == 0) { memcpy(&_desc_str[1], string_descriptors[0], 2); _desc_str[0] = (TUSB_DESC_STRING << 8) | 4; return _desc_str; }
        const char *str = string_descriptors[index];
        uint8_t len = strlen(str);
        if (len > 31) len = 31;
        for (uint8_t i = 0; i < len; i++) _desc_str[1 + i] = str[i];
        _desc_str[0] = (TUSB_DESC_STRING << 8) | (2 * len + 2);
        return _desc_str;
    }
}

// --- Logic ---
void initI2SDAC() {
    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = AUDIO_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8, .dma_buf_len = 256, .use_apll = true, .tx_desc_auto_clear = true
    };
    i2s_pin_config_t pins = { .bck_io_num = PIN_I2S_BCK, .ws_io_num = PIN_I2S_LCK, .data_out_num = PIN_I2S_DOUT, .data_in_num = -1 };
    i2s_driver_install(I2S_DAC_PORT, &cfg, 0, NULL);
    i2s_set_pin(I2S_DAC_PORT, &pins);
}

int16_t matrixMap(uint8_t x, uint8_t y) {
    if (x >= MATRIX_WIDTH || y >= MATRIX_HEIGHT) return -1;
    return (x % 2 == 0) ? (x * MATRIX_HEIGHT + y) : (x * MATRIX_HEIGHT + (MATRIX_HEIGHT - 1 - y));
}

struct BinRange { uint16_t start; uint16_t end; float weight; } barBins[NUM_BARS];

void calculateBinRanges() {
    float hzPerBin = (float)AUDIO_SAMPLE_RATE / FFT_SAMPLES;
    for (int i = 0; i < NUM_BARS; i++) {
        float low = 80.0f * powf(16000.0f / 80.0f, (float)i / NUM_BARS);
        float high = 80.0f * powf(16000.0f / 80.0f, (float)(i + 1) / NUM_BARS);
        barBins[i].start = max(1, (int)(low / hzPerBin));
        barBins[i].end = max((int)barBins[i].start, (int)(high / hzPerBin));
        barBins[i].weight = 1.0f / sqrtf(barBins[i].end - barBins[i].start + 1);
    }
}

RgbwColor getBarColor(uint8_t y, float bright) {
    float ratio = (float)y / MATRIX_HEIGHT;
    uint8_t r = (ratio < 0.5f) ? (uint8_t)(510 * ratio) : 255;
    uint8_t g = (ratio < 0.5f) ? 255 : (uint8_t)(255 - 510 * (ratio - 0.5f));
    return RgbwColor((uint8_t)(r * bright), (uint8_t)(g * bright), 0, 0);
}

void audioFFTTask(void *p) {
    static float hann[FFT_SAMPLES];
    for (int i = 0; i < FFT_SAMPLES; i++) hann[i] = 0.5f * (1.0f - cosf(2.0f * PI * i / (FFT_SAMPLES - 1)));
    
    TickType_t last = xTaskGetTickCount();
    while (1) {
        tud_task();
        if ((xTaskGetTickCount() - last) >= pdMS_TO_TICKS(16)) {
            last = xTaskGetTickCount();
            float gain = (analogRead(PIN_POT) / 4095.0f) * (GAIN_MAX - GAIN_MIN) + GAIN_MIN;
            DisplayFrame frame;
            frame.gain = gain;

            if (usbAudioActive) {
                uint16_t pos = ringWritePos;
                for (int i = 0; i < FFT_SAMPLES; i++) {
                    fftInput[i] = (float)audioRingBuffer[(pos + i) % FFT_SAMPLES] * hann[i];
                    fftOutput[i] = 0;
                }
                FFT.compute(FFTDirection::Forward);
                FFT.complexToMagnitude();

                for (int i = 0; i < NUM_BARS; i++) {
                    float mag = 0;
                    for (int b = barBins[i].start; b <= barBins[i].end; b++) mag += fftInput[b];
                    mag *= barBins[i].weight * gain;
                    float h = (mag > FFT_NOISE_FLOOR) ? log10f(mag) * MATRIX_HEIGHT / 3.0f : 0;
                    h = constrain(h, 0, MATRIX_HEIGHT);
                    
                    smoothedBars[i] = (h >= smoothedBars[i]) ? h : (smoothedBars[i] * BAR_DECAY_RATE);
                    if (smoothedBars[i] >= peakValues[i]) {
                        peakValues[i] = smoothedBars[i];
                        peakHoldCounters[i] = PEAK_HOLD_FRAMES;
                    } else if (peakHoldCounters[i] > 0) {
                        peakHoldCounters[i]--;
                    } else {
                        peakValues[i] = max(0.0f, peakValues[i] - PEAK_FALL_RATE);
                    }
                    frame.barHeights[i] = smoothedBars[i];
                    frame.peakPositions[i] = peakValues[i];
                }
            }
            xQueueOverwrite(displayQueue, &frame);
        }
        vTaskDelay(1);
    }
}

void ledMatrixTask(void *p) {
    strip.Begin();
    strip.Show();
    DisplayFrame frame;
    while (1) {
        if (xQueueReceive(displayQueue, &frame, pdMS_TO_TICKS(50))) {
            strip.ClearTo(RgbwColor(0,0,0,0));
            for (int x = 0; x < NUM_BARS; x++) {
                int h = (int)frame.barHeights[x];
                for (int y = 0; y < h; y++) {
                    int16_t idx = matrixMap(x, y);
                    if (idx >= 0) strip.SetPixelColor(idx, getBarColor(y, 0.8f));
                }
                int16_t pIdx = matrixMap(x, (int)frame.peakPositions[x]);
                if (pIdx >= 0) strip.SetPixelColor(pIdx, RgbwColor(30, 0, 0, 180));
            }
            strip.Show();
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void setup() {
    Serial0.begin(115200);
    analogReadResolution(12);
    initI2SDAC();
    calculateBinRanges();
    displayQueue = xQueueCreate(1, sizeof(DisplayFrame));
    USB.begin();
    
    xTaskCreatePinnedToCore(ledMatrixTask, "LED", 4096, NULL, 2, &ledTaskHandle, 0);
    xTaskCreatePinnedToCore(audioFFTTask, "FFT", 8192, NULL, 3, &audioTaskHandle, 1);
}

void loop() { vTaskDelay(pdMS_TO_TICKS(1000)); }