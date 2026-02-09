#ifndef __UI_MANAGER_H__
#define __UI_MANAGER_H__

#include <Arduino.h>
#include "oled.h"
#include "sys.h"
#include "battery.h"
#include "timerMetronome.h"

class UIManager
{
public:
    static void begin();
    static void update();
    static void onActivity();
    static void setLowBattery(bool isLow);
    static void resetScroll();
    static bool isScreenOn();

private:
    static void checkTimeout();
    static void drawStatusBar();
    static void drawKeyDesc();
    static void timerDisplay();

    // 屏幕超时参数
    static const uint32_t SCREEN_ALMOST_TIMEOUT = 5000;
    static const uint32_t SCREEN_TIMEOUT = 10000;

    // 状态变量
    static uint32_t lastActivityTime;
    static bool screenOn;
    static uint8_t scrollPos;
};

#endif
