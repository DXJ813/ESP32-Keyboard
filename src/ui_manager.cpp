#include "ui_manager.h"

// 静态成员变量定义
uint32_t UIManager::lastActivityTime = 0;
bool UIManager::screenOn = true;
uint8_t UIManager::scrollPos = 0;

void UIManager::begin()
{
    OLED_Init(7, 6, 32, 0);
    lastActivityTime = millis();
}

void UIManager::onActivity()
{
    // 如果有活动 (按键操作)，重置计时器并唤醒屏幕
    if (!BAT_IS_LOW)
    {
        OLED_LowBrightness(false);
    } // 恢复高亮度
    
    lastActivityTime = millis();
    
    if (!screenOn)
    {
        OLED_Power(true); // 唤醒 OLED 控制器
        screenOn = true;
    }
}

void UIManager::setLowBattery(bool isLow)
{
    if (isLow)
    {
        OLED_LowBrightness(true);
    }
}

void UIManager::resetScroll()
{
    scrollPos = 0;
}

bool UIManager::isScreenOn()
{
    return screenOn;
}

void UIManager::checkTimeout()
{
    static uint32_t lastCheck = 0;

    // 每 1000ms 检查一次超时状态
    if (millis() - lastCheck > 1000)
    {
        // 第一阶段：超时进入低亮度
        if (screenOn && (millis() - lastActivityTime > SCREEN_ALMOST_TIMEOUT))
        {
            OLED_LowBrightness(true);
        }
        // 第二阶段：超时关闭屏幕
        if (screenOn && (millis() - lastActivityTime > SCREEN_TIMEOUT))
        {
            OLED_Power(false);
            screenOn = false;
        }
        lastCheck = millis();
    }
}

void UIManager::update()
{
    if (!screenOn)
    {
        return;
    } // 屏幕关闭时不执行渲染，节省 I2C 总线资源

    checkTimeout();

    switch (currentMode)
    {
    case MODE_NORMAL:
        drawStatusBar();
        drawKeyDesc();
        break;

    case MODE_TIMER_SET:
        // 渲染定时器设置界面
        OLED_PrintText(0, 0, "> Timer Settings", 8);
        char timeStr[16];
        sprintf(timeStr, " <%02d:%02d>", timer.hours, timer.minutes);
        OLED_PrintText(0, 1, timeStr, 16);

        // 显示当前运行状态
        if (timer.enabled)
        {
            char timeEn[10];
            uint32_t remainingSec = (timer.targetSec - millis()) / 1000;
            sprintf(timeEn, "%02d:%02d[ON]", remainingSec / 3600, (remainingSec % 3600) / 60);
            OLED_PrintText(72, 1, timeEn, 8);
        }
        else
        {
            OLED_ClearPart(72, 1, 128, 2);
        }
        OLED_PrintText(72, 2, "Cnt Down", 8);
        OLED_PrintText(0, 3, "1|HH 2|MM 3|En 4|Rst", 8); // 操作指引
        
        timerDisplay(); // 此时其实不需要额外显示 timerDisplay，因为上面已经涵盖了逻辑，
                        // 但原代码逻辑似乎是 MIX 的，这里保持原样，或者根据原意调整。
                        // 原代码 TIMER_Display 只在 Normal 模式叠加显示。
                        // 这里 MODE_TIMER_SET 实际上是全屏显示设置。
        break;

    case MODE_METRONOME:
        // 渲染节拍器界面
        OLED_PrintText(0, 0, "> Metronome", 8);
        char infoStr[32];
        sprintf(infoStr, "BPM:%03d SIG:%d/4", metro.bpm, metro.timeSig);
        OLED_PrintText(0, 1, infoStr, 16);
        OLED_PrintText(0, 3, "1|- 2|+ 3|Sig 4|", 8);
        OLED_PrintText(96, 3, metro.isRunning ? "[RUN]" : "[OFF]", 8);
        break;

    case MODE_KEY_CONFIG:
        // 渲染配置选择界面 (支持滚动列表)
        static uint32_t lastScroll_cfg = 0;

        OLED_PrintText(0, 0, "> Config Mode", 8);
        if (changeName)
        {
            OLED_ClearPart(30, 1, 128, 2);
            changeName = false;
        }
        // 显示当前预设名称
        OLED_PrintText(0, 1, " Tag:", 8);
        OLED_PrintText(36, 1, (const char *)presets[currentPreset].name, 8);

        // 列表显示按键映射详情
        for (int i = 0; i < 2; i++)
        {
            if (scrollPos + i < 5)
            {
                char line[24];
                sprintf(line, "- Key%d: %s", (scrollPos + i + 1), presets[currentPreset].keyDescription[scrollPos + i]);
                OLED_PrintText(0, 2 + i, line, 8);
            }
        }
        // 列表自动滚动逻辑
        if (millis() - lastScroll_cfg > 2000)
        {
            scrollPos = (scrollPos + 1) % 4;
            lastScroll_cfg = millis();
            OLED_ClearPart(12, 2, 128, 4);
        }

        // 右上角页码
        char presetInfo[8];
        sprintf(presetInfo, "[%d/%d]", currentPreset + 1, PRESET_COUNT);
        OLED_PrintText(96, 0, presetInfo, 8);
        break;
    }
    
    // 原代码 TIMER_Display 是独立调用的，且只在 NORMAL 模式下显示（虽然 switch 外貌似没有判断）
    // 仔细看 main.cpp:211 行，TIMER_Display() 是在 case MODE_NORMAL: 里面调用的。
    // 所以在 update() 里，我们在 case MODE_NORMAL 调用即可。
    if (currentMode == MODE_NORMAL) {
        timerDisplay();
    }
}

void UIManager::drawStatusBar()
{
    // 顶部状态栏：标题、蓝牙图标、电量
    OLED_PrintImage(0, 0, 128, 1, (uint8_t *)Title);
    OLED_PrintImage(2, 1, 8, 1, (uint8_t *)BT);
    if (sysStatus.bleConnected)
    {
        OLED_PrintText(10, 1, "Connected  ", 8);
    }
    else
    {
        OLED_PrintText(10, 1, "Unconnected", 8);
    }

    // 电量显示 (右对齐)
    OLED_PrintImage(90, 1, 15, 1, (uint8_t *)Bat);
    OLED_PrintVar(100, 1, BAT_GetPercentage(), "int", 3);
    OLED_PrintText(118, 1, "%", 8);
}

void UIManager::drawKeyDesc()
{
    // 底部滚动显示按键描述 (Carousel Effect)
    // 由于屏幕较小，采用每 2 秒轮播一个按键功能描述的方式
    static uint32_t lastScroll = 0;
    if (scrollPos < 5)
    {
        char keydesc[24];
        sprintf(keydesc, "Key%d: %s", scrollPos + 1, presets[currentPreset].keyDescription[scrollPos]);
        OLED_PrintText(0, 2, keydesc, 8);
    }
    if (millis() - lastScroll > 2000)
    {
        scrollPos = (scrollPos + 1) % 5; // 循环索引 0-4
        lastScroll = millis();
        OLED_ClearPart(18, 2, 128, 4); // 局部清除区域
    }
}

void UIManager::timerDisplay()
{
    if (timer.enabled)
    {
        char timeStr[32];
        uint32_t remainingSec = (timer.targetSec - millis()) / 1000;
        sprintf(timeStr, "TIM remaining: %02d:%02d", remainingSec / 3600, (remainingSec % 3600) / 60);
        OLED_PrintText(0, 3, timeStr, 8);
    }
    else
    {
        OLED_ClearPart(0, 3, 128, 4);
    }
}
