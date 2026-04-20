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

#define ns 6 //number of stations max 9

String stations[ns]={
                "https://discodiamond.radioca.st/autodj",
                "https://listen.radioking.com/radio/175279/stream/216784",
                "http://sc6.radiocaroline.net:8040/stream",
                "https://club-high.rautemusik.fm/;",
                "http://greece-media.monroe.edu/wgmc.mp3",
                 "https://audio.radio-banovina.hr:9998/;"
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

  Serial.begin(9600, SERIAL_8N1);
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

  gfx->begin();
  gfx->fillScreen(RGB565_BLACK);

  analogWrite(GFX_BL,110);   //SCREEN BRIGHTNESS 0-255

  gfx->setCursor(2, 20);
  gfx->setTextSize(2);
  gfx->setTextColor(RGB565_GREEN);
  gfx->println("connecting to WI-FI");

  sprite.setColorDepth(16);     // RGB565
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

  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT, I2S_MCLK);
  audio.setVolume(volume*4); // 0...21
  audio.connecttohost(stations[0].c_str());
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
// ESP32-audioI2S callbacks. Kept silent so the serial CLI stays readable;
// the LCD already shows station / title / bitrate.
void audio_info(const char *info) { (void)info; }
void audio_id3data(const char *info) { (void)info; }

void audio_showstation(const char *info) {
  curStation = info;
  canDraw = true;
}
void audio_showstreamtitle(const char *info) {
  songPlaying = info;
  canDraw = 1;
}
void audio_bitrate(const char *info) {
  bitrate = (String(info).toInt() / 1000);
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
  audio.connecttohost(stations[chosen].c_str());
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


