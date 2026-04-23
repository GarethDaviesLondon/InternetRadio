#include "radio_audio.h"
#include "config.h"
#include "stations.h"
#include "storage.h"

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
// Volume lives on the 0..21 scale that ESP32-audioI2S accepts. The old
// 1..5 "big step" is derived from this via a rounding bucket so the
// on-screen bar and the buttons still behave.
static int      s_volRaw = 8;         // equivalent to the old "level 2"
static int8_t   s_bass   = 0;
static int8_t   s_mid    = 0;
static int8_t   s_treble = 0;
static String   s_curStation;
static String   s_song;
static long     s_bitrate = 0;
static unsigned s_infoCount = 0;
static bool     s_log = false;
static String   s_displayName;        // scratch for audioStationDisplayName

// Session persistence (NVS namespace "radio"). Keys:
//   vol  : int 0..21 (raw)
//   sta  : int current station index
//   bass/mid/trb : int8 EQ bands (-40..+6 dB)
static void persistSession() {
    storagePutInt("radio", "vol", s_volRaw);
    storagePutInt("radio", "sta", s_chosen);
    storagePutInt("radio", "bass", s_bass);
    storagePutInt("radio", "mid",  s_mid);
    storagePutInt("radio", "trb",  s_treble);
}

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
    // Keep the boot chirp quiet: the codec hasn't seen audioSetVolume yet so
    // anything we send plays at the codec's fixed 75/255 digital gain, which
    // is alarming at full scale. Scale the sample amplitude to "user volume
    // level 2" (2/5 of the 12000 full-amp reference).
    constexpr int     kMorseLevel = 2;                    // 1..5
    constexpr int16_t kMorseAmp   = (12000 * kMorseLevel) / 5;

    const float dphi = 2.0f * (float)M_PI * 700.0f / (float)AUDIO_SAMPLE_RATE;
    static float phase = 0.0f;
    int16_t buf[256];
    while (frames > 0) {
        uint32_t n = frames > 128 ? 128 : frames;
        for (uint32_t i = 0; i < n; i++) {
            int16_t s = 0;
            if (toneOn) {
                s = (int16_t)(sinf(phase) * (float)kMorseAmp);
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

    // R = dot dash dot. Target ~30 WPM: PARIS standard is 50 units per
    // word, so 30 WPM -> 1500 units/min -> 1 unit = 40 ms. Lead with 200 ms
    // of silence so the codec/PA finish their unmute ramp before the first
    // dot (otherwise it sounds like "N").
    const uint32_t U = 40, FR_PER_MS = AUDIO_SAMPLE_RATE / 1000;
    morseWriteFrames(tx, 200     * FR_PER_MS, false);
    morseWriteFrames(tx, U       * FR_PER_MS, true);
    morseWriteFrames(tx, U       * FR_PER_MS, false);
    morseWriteFrames(tx, (U * 3) * FR_PER_MS, true);
    morseWriteFrames(tx, U       * FR_PER_MS, false);
    morseWriteFrames(tx, U       * FR_PER_MS, true);
    morseWriteFrames(tx, 120     * FR_PER_MS, false);  // short tail, still avoids clip

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
    s_audio.setVolume(s_volRaw);
    s_audio.setTone(s_bass, s_mid, s_treble);
    return ok;
}

bool audioStartLast() {
    bool ok = s_audio.connecttohost(stationsUrl(s_chosen));
    Serial.printf("audio.connecttohost(slot %d) -> %s\r\n", s_chosen, ok ? "ok" : "FAIL");
    return ok;
}

void audioRestoreSession() {
    // Default 8 == old "level 2". If the saved value is <= 5 it's from an
    // older firmware that stored the 1..5 bucket rather than raw 0..21;
    // migrate it in-place.
    int v = storageGetInt("radio", "vol", 8);
    if (v <= 5) v = v * 4;
    if (v < 0)  v = 0; if (v > 21) v = 21;
    int s = storageGetInt("radio", "sta", 0);
    int n = stationsCount();
    if (n > 0) {
        if (s < 0)  s = 0;
        if (s >= n) s = n - 1;
    } else {
        s = 0;
    }
    s_volRaw = v;
    s_chosen = s;
    s_bass   = (int8_t)storageGetInt("radio", "bass", 0);
    s_mid    = (int8_t)storageGetInt("radio", "mid",  0);
    s_treble = (int8_t)storageGetInt("radio", "trb",  0);
    Serial.printf("audio: restored session -- volRaw=%d station=%d eq=%d/%d/%d\r\n",
                  s_volRaw, s_chosen, s_bass, s_mid, s_treble);
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
    persistSession();
    return ok;
}

void audioNextStation() { audioSelectStation((s_chosen + 1) % stationsCount()); }
void audioPrevStation() { audioSelectStation((s_chosen - 1 + stationsCount()) % stationsCount()); }

// Buttons + on-screen bar use a 1..5 "big step". Internally that maps to
// raw 4, 8, 12, 16, 20 on the audio library's 0..21 scale.
int  audioVolume() {
    int bucket = (s_volRaw + 3) / 4;
    if (bucket < 1) bucket = 1;
    if (bucket > 5) bucket = 5;
    return bucket;
}
void audioSetVolume(int v) {
    if (v < 1) v = 1;
    if (v > 5) v = 5;
    audioSetVolumeRaw(v * 4);
}

int  audioVolumeRaw() { return s_volRaw; }
void audioSetVolumeRaw(int raw) {
    if (raw < 0)  raw = 0;
    if (raw > 21) raw = 21;
    if (raw == s_volRaw) return;
    s_volRaw = raw;
    s_audio.setVolume(s_volRaw);
    persistSession();
}

void audioSetEq(int8_t bass, int8_t mid, int8_t treble) {
    // ESP32-audioI2S accepts -40..+6 dB on each band.
    auto clamp = [](int v) {
        if (v < -40) v = -40;
        if (v >   6) v =   6;
        return (int8_t)v;
    };
    s_bass   = clamp(bass);
    s_mid    = clamp(mid);
    s_treble = clamp(treble);
    s_audio.setTone(s_bass, s_mid, s_treble);
    persistSession();
}
int8_t audioEqBass()   { return s_bass;   }
int8_t audioEqMid()    { return s_mid;    }
int8_t audioEqTreble() { return s_treble; }

// --------------------------------------------------------------------------
// Observed state.
// --------------------------------------------------------------------------

const char *audioCurStation()    { return s_curStation.c_str(); }
const char *audioSongPlaying()   { return s_song.c_str(); }
long        audioBitrate()       { return s_bitrate; }
bool        audioIsRunning()     { return s_audio.isRunning(); }
unsigned    audioInfoEventCount(){ return s_infoCount; }

// Derive a tidy display name from the URL:
//   http://ice1.somafm.com/groovesalad-128-mp3  -> "groovesalad"
//   http://stream.radioparadise.com/mp3-128     -> "radioparadise"
//   http://sc6.radiocaroline.net:8040/stream    -> "stream"
// If the tail is useless ("stream", ";", "") fall back to the host.
// Result is truncated to fit the LCD.
const char *audioStationDisplayName(int idx) {
    String url = stationsUrl(idx);
    if (url.length() == 0) { s_displayName = ""; return s_displayName.c_str(); }

    int slashSlash = url.indexOf("://");
    int pathStart  = slashSlash >= 0 ? url.indexOf('/', slashSlash + 3) : 0;

    String host = (slashSlash >= 0 && pathStart > 0)
        ? url.substring(slashSlash + 3, pathStart)
        : url;
    int colon = host.indexOf(':');
    if (colon >= 0) host = host.substring(0, colon);
    // Strip a leading www. and take the second-level segment (radiocaroline
    // from sc6.radiocaroline.net) as a friendlier host name.
    if (host.startsWith("www.")) host = host.substring(4);
    int lastDot = host.lastIndexOf('.');
    int prevDot = (lastDot > 0) ? host.lastIndexOf('.', lastDot - 1) : -1;
    String hostShort = (prevDot >= 0) ? host.substring(prevDot + 1, lastDot) : host;

    String tail = (pathStart > 0) ? url.substring(pathStart + 1) : String();
    // Trim trailing slashes / semicolons.
    while (tail.length() &&
           (tail.endsWith("/") || tail.endsWith(";") || tail.endsWith("?"))) {
        tail.remove(tail.length() - 1);
    }
    // Drop leading path segments if the tail is nested (e.g. radioking).
    int lastSlash = tail.lastIndexOf('/');
    if (lastSlash >= 0) tail = tail.substring(lastSlash + 1);
    // Strip common suffixes / bitrate markers.
    for (const char *suf : { ".mp3", ".aac", ".ogg" }) {
        if (tail.endsWith(suf)) tail.remove(tail.length() - strlen(suf));
    }
    // If the tail contains "-128" / "-96" / "-64" / "_128" bitrate markers
    // drop them and everything after.
    for (int i = 0; i < (int)tail.length() - 2; i++) {
        char c0 = tail[i], c1 = tail[i + 1], c2 = tail[i + 2];
        if ((c0 == '-' || c0 == '_') && isDigit(c1) && isDigit(c2)) {
            tail = tail.substring(0, i);
            break;
        }
    }
    tail.trim();

    bool tailUseful = tail.length() > 0
                      && !tail.equalsIgnoreCase("stream")
                      && !tail.equalsIgnoreCase("listen")
                      && !tail.equalsIgnoreCase("autodj")
                      && !tail.equalsIgnoreCase("radio");
    // If the tail is purely digits, treat it as useless (e.g. radioking id).
    if (tailUseful) {
        bool allDigits = true;
        for (size_t i = 0; i < tail.length(); i++) {
            if (!isDigit(tail[i])) { allDigits = false; break; }
        }
        if (allDigits) tailUseful = false;
    }

    String out = tailUseful ? tail : hostShort;
    if (out.length() > 20) out = out.substring(0, 20);
    s_displayName = out;
    return s_displayName.c_str();
}
