#include "power.h"
#include "config.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"

static float g_volts = 4.20f;
static int   g_level = 0;

void powerBegin() {
    // Battery enable pin holds across deep sleep, so the divider is alive
    // when we wake. The hold is re-applied just before sleeping.
    gpio_hold_dis((gpio_num_t)PIN_BAT_EN);
    pinMode(PIN_BAT_EN, OUTPUT);
    digitalWrite(PIN_BAT_EN, HIGH);
    gpio_hold_en((gpio_num_t)PIN_BAT_EN);
}

void powerSampleBattery() {
    uint16_t mv = analogReadMilliVolts(PIN_BAT_ADC);
    float vbat = (mv / 1000.0f) * BAT_DIV_RATIO;
    g_volts = vbat;
    float pct = (vbat - BAT_V_MIN) / (BAT_V_MAX - BAT_V_MIN);
    pct = constrain(pct, 0.0f, 1.0f);
    g_level = (int)(pct * 13.0f);
}

float powerBatteryVolts() { return g_volts; }
int   powerBatteryLevel() { return g_level; }

void powerDeepSleep() {
    // Configure ext0 wake on the Left button going LOW. The regular GPIO
    // pull-up is disabled in deep sleep on ESP32-S3, so we must enable the
    // RTC pull-up explicitly or GPIO 0 floats and may either wake instantly
    // or never wake at all.
    gpio_num_t wakePin = (gpio_num_t)PIN_BTN_LEFT;
    rtc_gpio_init(wakePin);
    rtc_gpio_set_direction(wakePin, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(wakePin);
    rtc_gpio_pulldown_dis(wakePin);
    esp_sleep_enable_ext0_wakeup(wakePin, 0);

    // Quiesce the amp before the rails drop; otherwise there's an audible
    // click on power-down.
    digitalWrite(PIN_PA_CTRL, LOW);
    delay(200);
    esp_deep_sleep_start();
}
