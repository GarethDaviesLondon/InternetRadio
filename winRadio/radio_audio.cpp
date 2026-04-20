#include "radio_audio.h"
#include "config.h"
#include "stations.h"

#include <Arduino.h>
#include <Audio.h>
#include <Wire.h>
#include <math.h>
#include <driver/i2s_std.h>
#include "es8311.h"
#include "esp_check.h"
#include "esp_heap_caps.h"

// --------------------------------------------------------------------------
// Internal state.
// --------------------------------------------------------------------------

static Audio    s_audio;
static int      s_chosen = 0;
static int      s_volume = 2;
static String   s_curStation;
static String   s_song;
static long     s_bitrate = 0;
static unsigned s_infoCount = 0;
static bool     s_log = false;

// --------------------------------------------------------------------------
// Diagnostic toggle.
// --------------------------------------------------------------------------

bool audioLogEnabled()           { return s_log; }
void audioSetLogEnabled(bool on) { s_log = on; }

static void audioLog(const char *tag, const char *info) {
    if (!s_log) return;
    Serial.print("\r\n[audio ");
    Serial.print(tag);
    Serial.print("] ");
    Serial.print(info ? info : "");
    Serial.print("\r\nradio> ");
}

// --------------------------------------------------------------------------
// Audio library event dispatch (Audio::audio_info_callback).
// --------------------------------------------------------------------------

static void audioEventHandler(Audio::msg_t msg) {
    const char *p = msg.msg ? msg.msg : "";
    switch (msg.e) {
        case Audio::evt_name:
            s_curStation = p;
            audioLog("station", p); break;
        case Audio::evt_streamtitle:
            s_song = p;
            audioLog("title", p); break;
        case Audio::evt_bitrate:
            s_bitrate = String(p).toInt() / 1000;
            audioLog("bitrate", p); break;
        case Audio::evt_info:
            s_infoCount++;
            audioLog("info", p); break;
        case Audio::evt_id3data: audioLog("id3", p); break;
        case Audio::evt_eof:     audioLog("eof", p); break;
        case Audio::evt_log:     audioLog("log", p); break;
        default:
            audioLog(msg.s ? msg.s : "?", p); break;
    }
}

// --------------------------------------------------------------------------
// ES8311 codec init.
// --------------------------------------------------------------------------

bool audioCodecInit() {
    pinMode(PIN_PA_CTRL, OUTPUT);
    digitalWrite(PIN_PA_CTRL, HIGH);

    es8311_handle_t es = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
    if (!es) {
        Serial.println("audio: es8311_create FAILED");
        return false;
    }
    const es8311_clock_config_t clk = {
        .mclk_inverted     = false,
        .sclk_inverted     = false,
        .mclk_from_mclk_pin= true,
        .mclk_frequency    = AUDIO_MCLK_FREQ_HZ,
        .sample_frequency  = AUDIO_SAMPLE_RATE,
    };
    if (es8311_init(es, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) {
        Serial.println("audio: es8311_init FAILED");
        return false;
    }
    es8311_sample_frequency_config(es,
        AUDIO_SAMPLE_RATE * AUDIO_MCLK_MULTIPLE, AUDIO_SAMPLE_RATE);
    es8311_voice_volume_set(es, AUDIO_CODEC_DEF_VOL, NULL);
    es8311_microphone_config(es, false);
    return true;
}

// --------------------------------------------------------------------------
// Morse "R" speaker self-test. Drives the ES8311 directly via the ESP-IDF
// i2s_std driver so it can run before the Audio library claims I2S 0.
// --------------------------------------------------------------------------

static void morseWriteFrames(i2s_chan_handle_t h, uint32_t frames, bool toneOn) {
    const float dphi = 2.0f * (float)M_PI * 700.0f / (float)AUDIO_SAMPLE_RATE;
    static float phase = 0.0f;
    int16_t buf[256];
    while (frames > 0) {
        uint32_t n = frames > 128 ? 128 : frames;
        for (uint32_t i = 0; i < n; i++) {
            int16_t s = 0;
            if (toneOn) {
                s = (int16_t)(sinf(phase) * 12000.0f);
                phase += dphi;
                if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
            }
            buf[i*2]   = s;
            buf[i*2+1] = s;
        }
        size_t written = 0;
        i2s_channel_write(h, buf, n * 4, &written, portMAX_DELAY);
        frames -= n;
    }
}

void audioPlayMorseR() {
    Serial.printf("morse: DMA-capable heap free = %u bytes\r\n",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));

    i2s_chan_handle_t tx = nullptr;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num  = 2;
    cc.dma_frame_num = 128;

    if (i2s_new_channel(&cc, &tx, nullptr) != ESP_OK) {
        Serial.println("morse: i2s_new_channel FAILED");
        return;
    }

    i2s_std_config_t sc = {};
    sc.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE);
    sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    sc.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    sc.gpio_cfg.mclk = (gpio_num_t)PIN_I2S_MCLK;
    sc.gpio_cfg.bclk = (gpio_num_t)PIN_I2S_BCLK;
    sc.gpio_cfg.ws   = (gpio_num_t)PIN_I2S_LRC;
    sc.gpio_cfg.dout = (gpio_num_t)PIN_I2S_DOUT;
    sc.gpio_cfg.din  = I2S_GPIO_UNUSED;

    if (i2s_channel_init_std_mode(tx, &sc) != ESP_OK) {
        Serial.println("morse: init_std_mode FAILED");
        i2s_del_channel(tx); return;
    }
    if (i2s_channel_enable(tx) != ESP_OK) {
        Serial.println("morse: channel_enable FAILED");
        i2s_del_channel(tx); return;
    }

    // R = dot dash dot. 200 ms lead silence so the codec/PA finish their
    // unmute ramp before the first dot (otherwise it sounds like "N").
    const uint32_t U = 120, FR_PER_MS = AUDIO_SAMPLE_RATE / 1000;
    morseWriteFrames(tx, 200     * FR_PER_MS, false);
    morseWriteFrames(tx, U       * FR_PER_MS, true);
    morseWriteFrames(tx, U       * FR_PER_MS, false);
    morseWriteFrames(tx, (U * 3) * FR_PER_MS, true);
    morseWriteFrames(tx, U       * FR_PER_MS, false);
    morseWriteFrames(tx, U       * FR_PER_MS, true);
    morseWriteFrames(tx, 200     * FR_PER_MS, false);

    i2s_channel_disable(tx);
    i2s_del_channel(tx);
    Serial.println("morse: done");
}

// --------------------------------------------------------------------------
// Audio library lifecycle.
// --------------------------------------------------------------------------

bool audioBegin() {
    Serial.printf("pre-audio DMA heap free = %u bytes\r\n",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    Audio::audio_info_callback = audioEventHandler;
    bool ok = s_audio.setPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT, PIN_I2S_MCLK);
    Serial.printf("audio.setPinout -> %s\r\n", ok ? "ok" : "FAIL");
    s_audio.setVolume(s_volume * 4);
    return ok;
}

bool audioStartDefault() {
    bool ok = s_audio.connecttohost(stationsUrl(s_chosen));
    Serial.printf("audio.connecttohost -> %s\r\n", ok ? "ok" : "FAIL");
    return ok;
}

void audioLoop() { s_audio.loop(); }

// --------------------------------------------------------------------------
// Playback control.
// --------------------------------------------------------------------------

int audioCurrentStation() { return s_chosen; }

bool audioSelectStation(int idx) {
    int n = stationsCount();
    if (n <= 0) return false;
    if (idx < 0)  idx = 0;
    if (idx >= n) idx = n - 1;
    s_chosen = idx;
    bool ok = s_audio.connecttohost(stationsUrl(s_chosen));
    Serial.printf("connecttohost('%s') -> %s\r\n", stationsUrl(s_chosen), ok ? "ok" : "FAIL");
    return ok;
}

void audioNextStation() { audioSelectStation((s_chosen + 1) % stationsCount()); }
void audioPrevStation() { audioSelectStation((s_chosen - 1 + stationsCount()) % stationsCount()); }

int  audioVolume() { return s_volume; }
void audioSetVolume(int v) {
    if (v < 1) v = 1;
    if (v > 5) v = 5;
    s_volume = v;
    s_audio.setVolume(s_volume * 4);
}

// --------------------------------------------------------------------------
// Observed state.
// --------------------------------------------------------------------------

const char *audioCurStation()    { return s_curStation.c_str(); }
const char *audioSongPlaying()   { return s_song.c_str(); }
long        audioBitrate()       { return s_bitrate; }
bool        audioIsRunning()     { return s_audio.isRunning(); }
unsigned    audioInfoEventCount(){ return s_infoCount; }
