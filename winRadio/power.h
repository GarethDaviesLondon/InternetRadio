// Power: battery measurement, deep sleep, supply hold-on.

#pragma once

#include <Arduino.h>

void  powerBegin();           // sets up battery enable pin + holds it across sleep
void  powerSampleBattery();   // call ~every 240 ms; updates voltage / level
float powerBatteryVolts();
int   powerBatteryLevel();    // 0..13 (matches the on-screen bar)

void  powerDeepSleep();       // turns off the PA and sleeps; left button wakes
