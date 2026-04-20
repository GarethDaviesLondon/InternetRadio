#include "Arduino.h"
#include "WiFiMulti.h"
#include "Audio.h"
#include "SD_MMC.h"
#include "FS.h"
#include <Arduino_GFX_Library.h>
#include <LovyanGFX.hpp>
#include "es8311.h"
#include "esp_check.h"
#include "Wire.h"
#include "NotoSansBold15.h"
#include "cli.h"
#include <math.h>
#include <driver/i2s_std.h>

// Colour-name compatibility with the original Volos sketch. Newer releases
// of Arduino_GFX / LovyanGFX only ship the RGB565_* and TFT_* variants.
#ifndef BLACK
  #define BLACK  0x0000
#endif
#ifndef YELLOW
  #define YELLOW 0xFFE0
#endif
#ifndef ORANGE
  #define ORANGE 0xFD20
#endif

#define PA_CTRL 7
#define I2S_MCLK 8
#define I2S_BCLK 9
#define I2S_DOUT 12
#define I2S_LRC 10

#define I2C_SDA 42
#define I2C_SCL 41

#define EXAMPLE_SAMPLE_RATE (16000)
#define EXAMPLE_MCLK_MULTIPLE (256)  // If not using 24-bit data width, 256 should be enough
#define EXAMPLE_MCLK_FREQ_HZ (EXAMPLE_SAMPLE_RATE * EXAMPLE_MCLK_MULTIPLE)
#define EXAMPLE_VOICE_VOLUME (75)

LGFX_Sprite sprite; 
LGFX_Sprite sprite2; 

String curStation="";
String songPlaying="";
long bitrate=0;
bool connected=false;
int songposition=-220;
float voltage=4.20;
int batLevel=0;

Audio audio;
WiFiMulti wifiMulti;
// WiFi credentials live in NVS (see cli.ino). Set them via the serial CLI
// at 9600 8N1: type `wifi` and follow the SSID / password prompts.

bool canDraw=0;
bool deb=0;
bool deb2=0;
int rssi=0;

int clk = 16;
int cmd = 15;
int d0 = 17;
int d1 = 18;
int d2 = 13;
int d3 = 14;


int chosen=0; //current station
int volume=2;
String letters[3]={"P","S","V"};

unsigned short grays[18];
unsigned short gray;
unsigned short light;

int g[14]={0};  //graph

#define ns 8 //number of stations max 9

String stations[ns]={
                "http://ice1.somafm.com/groovesalad-128-mp3",
                "https://discodiamond.radioca.st/autodj",
                "https://listen.radioking.com/radio/175279/stream/216784",
                "http://sc6.radiocaroline.net:8040/stream",
                "https://club-high.rautemusik.fm/;",
                "http://greece-media.monroe.edu/wgmc.mp3",
                "https://audio.radio-banovina.hr:9998/;",
                "http://stream.radioparadise.com/mp3-128"
                };


#define GFX_BL 46
Arduino_DataBus* bus = new Arduino_ESP32SPI(45 /* DC */, 21 /* CS */, 38 /* SCK */, 39 /* MOSI */, -1 /* MISO */);
Arduino_GFX* gfx = new Arduino_ST7789(
bus, 40 /* RST */, 0 /* rotation */, true, 240, 240);                

static esp_err_t es8311_codec_init(void) {

  es8311_handle_t es_handle = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
  ESP_RETURN_ON_FALSE(es_handle, ESP_FAIL, TAG, "es8311 create failed");
  const es8311_clock_config_t es_clk = {
    .mclk_inverted = false,
    .sclk_inverted = false,
    .mclk_from_mclk_pin = true,
    .mclk_frequency = EXAMPLE_MCLK_FREQ_HZ,
    .sample_frequency = EXAMPLE_SAMPLE_RATE
  };

  ESP_ERROR_CHECK(es8311_init(es_handle, &es_clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16));
  ESP_RETURN_ON_ERROR(es8311_sample_frequency_config(es_handle, EXAMPLE_SAMPLE_RATE * EXAMPLE_MCLK_MULTIPLE, EXAMPLE_SAMPLE_RATE), TAG, "set es8311 sample frequency failed");
  ESP_RETURN_ON_ERROR(es8311_voice_volume_set(es_handle, EXAMPLE_VOICE_VOLUME, NULL), TAG, "set es8311 volume failed");
  ESP_RETURN_ON_ERROR(es8311_microphone_config(es_handle, false), TAG, "set es8311 microphone failed");

  return ESP_OK;
}


void setup() {

  Serial.begin(9600);  // HWCDC: baud is ignored (virtualised over USB)
  // USB CDC re-enumerates after reset; wait briefly so the banner isn't
  // swallowed. Cap at 1.5 s so a headless boot isn't blocked.
  {
    unsigned long t0 = millis();
    while (!Serial && millis() - t0 < 1500) delay(10);
  }
  cliBegin();
  loadWifiCreds();
  Wire.begin(I2C_SDA, I2C_SCL);
  gpio_hold_dis((gpio_num_t)2);
  pinMode(0, INPUT_PULLUP); // left na GPIO0
  pinMode(5, INPUT_PULLUP); // mid button
  pinMode(4, INPUT_PULLUP); // right button

  // batt enable
  pinMode(2,OUTPUT);
  digitalWrite(2,HIGH);

  pinMode(PA_CTRL, OUTPUT);
  digitalWrite(PA_CTRL, HIGH);
  es8311_codec_init();
  gpio_hold_en((gpio_num_t)2);

  // Speaker self-test: dot-dash-dot (Morse "R") before the Audio library
  // takes over I2S 0. If you hear this, the codec + amp + speaker path is
  // healthy and any silence afterwards is a streaming issue, not hardware.
  radioPlayMorseR();

  gfx->begin();
  gfx->fillScreen(RGB565_BLACK);

  analogWrite(GFX_BL,110);   //SCREEN BRIGHTNESS 0-255

  gfx->setCursor(2, 20);
  gfx->setTextSize(2);
  gfx->setTextColor(RGB565_GREEN);
  gfx->println("connecting to WI-FI");

  sprite.setColorDepth(16);     // RGB565
  // Put the 115 KB 240x240 sprite in PSRAM; otherwise it eats internal SRAM
  // and starves the Audio library's I2S DMA allocation at boot.
  sprite.setPsram(true);
  sprite.createSprite(240, 240);
  sprite2.createSprite(230, 16);
  
  sprite.loadFont(NotoSansBold15);

     int co = 214;
    for (int i = 0; i < 18; i++) {
    grays[i] = sprite.color565(co, co, co+40);
    co = co - 13;
    }

  sprite2.setTextColor(grays[0],TFT_BLACK);

  WiFi.mode(WIFI_STA);
  if (!hasWifiCreds()) {
    gfx->fillScreen(RGB565_BLACK);
    gfx->setCursor(2, 20);
    gfx->setTextSize(2);
    gfx->setTextColor(RGB565_YELLOW);
    gfx->println("No WiFi creds.");
    gfx->println("Connect serial");
    gfx->println("@ 9600 8N1");
    gfx->println("and type: wifi");
    cliFirstRunSetup();
  }
  wifiMulti.addAP(getWifiSsid(), getWifiPassword());
  wifiMulti.run();
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect(true);
    wifiMulti.run();
  }

  Serial.printf("pre-audio DMA heap free = %u bytes\r\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
  Audio::audio_info_callback = audioEventHandler;   // must be set before connect
  bool pinOk = audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT, I2S_MCLK);
  Serial.printf("audio.setPinout -> %s\r\n", pinOk ? "ok" : "FAIL");
  audio.setVolume(volume*4); // 0...21
  bool connOk = audio.connecttohost(stations[0].c_str());
  Serial.printf("audio.connecttohost -> %s\r\n", connOk ? "ok" : "FAIL");
}

void draw2()
{

sprite.drawString("Hello",20,20,2);

gray=grays[16];
        light=grays[12];
        sprite.fillRect(0,0,240,240,gray);
        
        //stations frame
        sprite.fillRect(4,20,150,172,BLACK);
        sprite.drawRect(4,20,150,172,light);

        // time and grapg frame
        sprite.fillRect(160,20,74,60,BLACK);
        sprite.drawRect(160,20,74,60,light);
        sprite.fillRect(174,24,5,10,TFT_RED);
        sprite.fillRect(174,37,5,10,TFT_GREEN);
       

        //battery
          sprite.drawRect(210,36,17,10,TFT_GREEN);
          sprite.fillRect(212,38,batLevel,6,TFT_GREEN); //bat lvl
          sprite.fillRect(227,39,2,4,TFT_GREEN);

        //bitrate
        sprite.fillRect(160,176,74,16,BLACK);
        sprite.drawRect(160,176,74,16,light);

           //volume bar
        sprite.fillRoundRect(160,140,74,3,2,YELLOW);
        sprite.fillRoundRect(146+(volume*15),137,14,8,2,grays[2]);
        sprite.fillRoundRect(149+(volume*15),139,8,4,2,grays[10]);

         //songplaying frame
        sprite.fillRect(4,212,232,18,BLACK);
        sprite.drawRect(4,212,232,18,light);

        sprite.fillRect(149,20,5,172,grays[11]);

        int sliderPos=12;
        sprite.fillRect(149,sliderPos+8,5,20,grays[2]);
        sprite.fillRect(151,sliderPos+12,1,12,grays[16]);

        sprite.fillRect(4,7,150,3,ORANGE);
       
        sprite.fillRect(190,5,45,3,ORANGE);
        sprite.fillRect(160,194,74,1,ORANGE);
        sprite.fillRect(190,11,45,3,grays[6]);
        
       

        //frame top and bot
        sprite.drawRect(0,0,239,239,light);
        sprite.fillRect(5,234,230,2,grays[13]);
        

       sprite.setTextColor(grays[1],gray);
       sprite.drawString(" STATIONS ",42,2,2);
       sprite.drawString("WEB",160,2,2);

        //station list
        for(int i=0;i<ns;i++)
        {
        if(i==chosen) sprite.setTextColor(TFT_GREEN,TFT_BLACK); else  sprite.setTextColor(TFT_DARKGREEN,TFT_BLACK);
        sprite.drawString(stations[i].substring(0,20),10,26+(i*19),2);
        }

        sprite.setTextColor(grays[0],gray);
        sprite.drawString("INTERNET",160,86); 
        sprite.setTextColor(TFT_RED,gray);
        sprite.drawString("RADIO",160,102); 

        sprite.setTextColor(grays[6],gray);
        sprite.drawString("SONG PLAYING",6,200,1); 
        sprite.drawString("VOLUME",160,124,1);
         sprite.setTextColor(grays[10],TFT_BLACK); 
         sprite.drawString("W",165,24,1); 
         sprite.drawString("I",165,34,1); 
         sprite.drawString("F",165,44,1); 
         sprite.drawString("I",165,54,1); 
         sprite.setTextColor(TFT_GREEN,TFT_BLACK); 
         sprite.drawString("BITRATE "+String(bitrate),164,180,1); 
         sprite.drawString("RSSI:"+String(rssi),183,24,1); 
          sprite.drawString(String(voltage),183,37,1); 
         
         sprite.setTextColor(grays[11],gray); 
         sprite.drawString("VOLOS PROJECTS 2026",122,200,1); 

        //graph
        
        for(int i=0;i<12;i++){  
        if(connected)
        g[i]=random(1,5);
        for(int j=0;j<g[i];j++)
        sprite.fillRect(172+(i*5),71-j*4,4,3,grays[4]);
        }
   

        sprite.setTextColor(grays[16],grays[5]);
        //butons
        for(int i=0;i<3;i++)
        {
          sprite.fillRoundRect(160+(i*26),152,22,18,4,grays[5]);
          sprite.drawString(letters[i],166+(i*26),154); 
        }

   

uint16_t *buf = (uint16_t*)sprite.getBuffer();
int total = 240 * 240;

for (int i = 0; i < total; i++) {
    buf[i] = __builtin_bswap16(buf[i]);
}

gfx->draw16bitRGBBitmap(0, 0, buf, 240, 240);

canDraw=0;
draw3();
}




void draw3()
{
     songposition--;
     if(songposition<-220) songposition=220;
     sprite2.fillSprite(TFT_BLACK);  
     sprite2.drawString(songPlaying,songposition,5);  

    uint16_t *buf2 = (uint16_t*)sprite2.getBuffer();
    int total2 = 230 * 16;

   for (int i = 0; i < total2; i++) {
    buf2[i] = __builtin_bswap16(buf2[i]);
   }

   gfx->draw16bitRGBBitmap(5, 213, buf2, 230, 16);
}

void measureBatt()
{
    uint16_t mv = analogReadMilliVolts(1);  // mV na ADC pinu
    float vbat = (mv / 1000.0) * 3.0;             // stvarni napon baterije

    voltage = vbat;
    char vol_buffer[8];
    sprintf(vol_buffer, "%.2f", voltage);

    // izračun postotka baterije (Li-ion)
    float minV = 3.0;
    float maxV = 4.2;

    float pct = (vbat - minV) / (maxV - minV);
    pct = constrain(pct, 0.0, 1.0);

    batLevel = pct * 13.0;
}

void loop() {



  //measure signal strength
  static unsigned long lastRSSI = 0;
   static unsigned long lastSlide = 0;

if (millis() - lastRSSI > 240) {   // every 240 ms
    lastRSSI = millis();
    rssi = WiFi.RSSI();  // očitaj jačinu signala
    measureBatt();
    canDraw=1;

    if (WiFi.status() == WL_CONNECTED)
    {connected=true;}
    else
    {connected=false;
    songPlaying="WIFI NOT CONNECTED";}
}

if (millis() - lastSlide > 30) {   // svakih 1 sekundu
    lastSlide = millis();
    draw3();
}


  if (digitalRead(5) == LOW) {
  if(deb==0)
      {
        deb=1;
        chosen++;
        if(chosen==ns) chosen=0;
        audio.connecttohost(stations[chosen].c_str());
        canDraw=1;
      }
  }else deb=0;


    if (digitalRead(4) == LOW) {
  if(deb2==0)
      {
        deb2=1;
        volume++;
        if(volume==6) volume=1;
        audio.setVolume(volume*4);
        canDraw=1;
      }
  }else deb2=0;
  
    if (digitalRead(0) == LOW) {
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0); // probudi se kad gumb opet bude LOW
    digitalWrite(PA_CTRL, LOW);
    delay(200);
    esp_deep_sleep_start();
  }

  cliPoll();

  vTaskDelay(1);
  audio.loop();

   if(canDraw)
   draw2();

}

// optional
// --------------------------------------------------------------------------
// Morse "R" speaker self-test. Drives the ES8311 directly via the ESP-IDF
// i2s_std driver so it can run before the Audio library claims I2S 0.
// --------------------------------------------------------------------------

static void morseWriteFrames(i2s_chan_handle_t h, uint32_t frames, bool toneOn) {
  const uint32_t SR = 16000;             // must match es8311_codec_init
  const float    HZ = 700.0f;
  const float    dphi = 2.0f * (float)M_PI * HZ / SR;
  static float   phase = 0.0f;
  int16_t        buf[256];               // 128 stereo frames
  while (frames > 0) {
    uint32_t n = frames > 128 ? 128 : frames;
    for (uint32_t i = 0; i < n; i++) {
      int16_t s = 0;
      if (toneOn) {
        s = (int16_t)(sinf(phase) * 12000.0f);
        phase += dphi;
        if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
      }
      buf[i * 2]     = s;
      buf[i * 2 + 1] = s;
    }
    size_t written = 0;
    i2s_channel_write(h, buf, n * 4, &written, portMAX_DELAY);
    frames -= n;
  }
}

void radioPlayMorseR() {
  const size_t dmaFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  Serial.printf("morse: DMA-capable heap free = %u bytes\r\n", (unsigned)dmaFree);

  i2s_chan_handle_t tx = nullptr;
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  // Shrink DMA footprint: default is 6 × 240 frames = 5.8 KB of internal
  // DMA-capable SRAM, which some arduino-esp32 builds can't satisfy this
  // early in boot. 2 × 128 frames × 4 B/frame = 1 KB total.
  cc.dma_desc_num  = 2;
  cc.dma_frame_num = 128;

  esp_err_t err = i2s_new_channel(&cc, &tx, nullptr);
  if (err != ESP_OK) {
    Serial.printf("morse: i2s_new_channel failed (0x%x)\r\n", err);
    return;
  }

  i2s_std_config_t sc = {};
  sc.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000);
  sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  sc.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  sc.gpio_cfg.mclk = (gpio_num_t)I2S_MCLK;
  sc.gpio_cfg.bclk = (gpio_num_t)I2S_BCLK;
  sc.gpio_cfg.ws   = (gpio_num_t)I2S_LRC;
  sc.gpio_cfg.dout = (gpio_num_t)I2S_DOUT;
  sc.gpio_cfg.din  = I2S_GPIO_UNUSED;

  err = i2s_channel_init_std_mode(tx, &sc);
  if (err != ESP_OK) {
    Serial.printf("morse: init_std_mode failed (0x%x)\r\n", err);
    i2s_del_channel(tx);
    return;
  }

  err = i2s_channel_enable(tx);
  if (err != ESP_OK) {
    Serial.printf("morse: channel_enable failed (0x%x)\r\n", err);
    i2s_del_channel(tx);
    return;
  }

  // R = dot dash dot, 120 ms per unit. Lead with 200 ms of silence so the
  // codec / PA finish their unmute ramp before the first dot; without it
  // the opening dot is swallowed and you hear "N" (dash-dot) instead of R.
  const uint32_t U = 120;                // dot length in ms
  const uint32_t FR_PER_MS = 16;         // 16000 Hz / 1000
  morseWriteFrames(tx, 200     * FR_PER_MS, false);  // warm-up
  morseWriteFrames(tx, U       * FR_PER_MS, true);   // .
  morseWriteFrames(tx, U       * FR_PER_MS, false);
  morseWriteFrames(tx, (U * 3) * FR_PER_MS, true);   // -
  morseWriteFrames(tx, U       * FR_PER_MS, false);
  morseWriteFrames(tx, U       * FR_PER_MS, true);   // .
  morseWriteFrames(tx, 200     * FR_PER_MS, false);  // tail so last dot isn't clipped

  i2s_channel_disable(tx);
  i2s_del_channel(tx);
  Serial.println("morse: done");
}

// ESP32-audioI2S callbacks. State updates always run; the serial log is
// gated on the CLI's `log` toggle (default off) so it doesn't clobber the
// prompt while you're typing.
// Audio library events.
// The current ESP32-audioI2S (>= 2024 major rewrite) no longer exposes weak
// audio_info / audio_bitrate / audio_showstation functions. Instead it
// dispatches everything through a single std::function set as
// Audio::audio_info_callback, with a msg_t carrying an event_t enum and a
// payload string in msg.msg. We switch on msg.e to update state and
// optionally mirror the payload to Serial when the CLI `log` is on.

static volatile unsigned g_audioInfoCount = 0;
unsigned radioAudioInfoCount() { return g_audioInfoCount; }

static void audioLog(const char *tag, const char *info) {
  if (!audioLogEnabled()) return;
  Serial.print("\r\n[audio ");
  Serial.print(tag);
  Serial.print("] ");
  Serial.print(info ? info : "");
  Serial.print("\r\nradio> ");
}

static void audioEventHandler(Audio::msg_t msg) {
  const char *p = msg.msg ? msg.msg : "";
  switch (msg.e) {
    case Audio::evt_name:
      curStation = p; canDraw = true;
      audioLog("station", p); break;
    case Audio::evt_streamtitle:
      songPlaying = p; canDraw = 1;
      audioLog("title", p); break;
    case Audio::evt_bitrate:
      bitrate = String(p).toInt() / 1000;
      audioLog("bitrate", p); break;
    case Audio::evt_info:
      g_audioInfoCount++;
      audioLog("info", p); break;
    case Audio::evt_id3data:
      audioLog("id3", p); break;
    case Audio::evt_eof:
      audioLog("eof", p); break;
    case Audio::evt_log:
      audioLog("log", p); break;
    default:
      audioLog(msg.s ? msg.s : "?", p); break;
  }
}

// --------------------------------------------------------------------------
// Radio control API used by the serial CLI (see cli.h / cli.cpp).
// --------------------------------------------------------------------------

int         radioStationCount()         { return ns; }
const char *radioStationUrl(int idx)    { return stations[idx].c_str(); }
int         radioCurrentStation()       { return chosen; }

void radioSelectStation(int idx) {
  if (idx < 0) idx = 0;
  if (idx >= ns) idx = ns - 1;
  chosen = idx;
  bool ok = audio.connecttohost(stations[chosen].c_str());
  Serial.printf("connecttohost('%s') -> %s\r\n",
                stations[chosen].c_str(), ok ? "ok" : "FAIL");
  canDraw = 1;
}

void radioNextStation() { radioSelectStation((chosen + 1) % ns); }
void radioPrevStation() { radioSelectStation((chosen - 1 + ns) % ns); }

int  radioVolume() { return volume; }
void radioSetVolume(int v) {
  if (v < 1) v = 1;
  if (v > 5) v = 5;
  volume = v;
  audio.setVolume(volume * 4);
  canDraw = 1;
}

long        radioBitrate()     { return bitrate; }
float       radioBattery()     { return voltage; }
const char *radioSongPlaying() { return songPlaying.c_str(); }
bool        radioIsRunning()   { return audio.isRunning(); }

void radioReconnectWifi() {
  WiFi.disconnect(true);
  delay(100);
  wifiMulti.addAP(getWifiSsid(), getWifiPassword());
  wifiMulti.run();
}

void radioDeepSleep() {
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
  digitalWrite(PA_CTRL, LOW);
  delay(200);
  esp_deep_sleep_start();
}


