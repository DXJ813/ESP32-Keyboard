/******************************************************************************
 * @file    main.cpp
 * @brief   ESP32-C3 BLE Keybrick 主程序
 * @author  WilliTourt
 * @date    2025
 * @note    蓝牙mini键盘
 ******************************************************************************/

#include <Arduino.h>
#include "Hid2Ble.h"

/* 引入 C 语言编写的底层驱动库。
 * 使用 extern "C" 告诉 C++ 编译器按照 C 语言规范进行链接，
 * 防止因函数名修饰（Name Mangling）导致链接错误。
 */
extern "C"
{
#include "key.h"
#include "oled.h"
#include "battery.h"
}

#include "sys.h"
#include "def.h"
#include "timerMetronome.h"

// =================================================================================
// 宏定义与全局变量
// =================================================================================

/* 屏幕超时设置 (单位: ms) */
#define SCREEN_ALMOST_TIMEOUT 5000 // 5秒无操作进入低亮度模式
#define SCREEN_TIMEOUT 10000       // 10秒无操作关闭屏幕

void OLED_Update();
void OLED_ChkTimeout();
void KEY_Send();

/* 实例化 BLE HID 对象: 设备名, 制造商, 电量初始值 */
Hid2Ble keybrick("ESP32C3 BLE Keybrick", "dxj", 100);

// =================================================================================
// 中断服务程序
// =================================================================================
/**
 * @brief  按键检测与系统定时器中断服务函数
 * @note   触发频率: 10ms (100Hz)
 * @note   IRAM_ATTR 属性强制将此函数加载到 IRAM 中运行，
 * 避免 Flash 缓存未命中（Cache Miss）导致的中断延迟，保证实时性。
 */
void IRAM_ATTR KEY_Detect()
{

    // --- 1. 按键状态扫描与去抖逻辑 ---
    // 遍历 5 个物理按键
    for (int i = 0; i < 5; i++)
    {
        if (keyState[i].isReleased)
        {
            // 逻辑：当按键被物理释放，且之前处于按下状态时，标记发送“释放”信号
            if (!keyState[i].isPressed)
            {
                sendRelease = true;             // 全局标志位：请求发送 Key Release
                keyState[i].isReleased = false; // 复位状态
            }
        }
        else
        {
            // 逻辑：当按键处于按下状态，标记 shouldSend 请求主循环发送键值
            if (keyState[i].isPressed)
            {
                keyState[i].shouldSend = true;
            }
        }
    }

    // --- 2. 倒计时定时器逻辑 (Timer Mode) ---
    // 通过累加 10ms 中断次数来实现秒级计时
    static uint8_t timerCnt = 0;
    if (timer.enabled)
    {
        timerCnt++;
        if (timerCnt >= 100)
        { // 10ms * 100 = 1000ms = 1s
            uint32_t now = millis();
            // 检查是否到达目标时间
            if (now >= (timer.targetSec - 1))
            {
                timerTriggered = true; // 触发定时器结束事件
                timer.enabled = false; // 停止计时
            }
            timerCnt = 0; // 重置秒计数器
        }
    }
}

/**
 * @brief  电池电量更新中断
 * @note   触发频率: 1分钟
 */
void IRAM_ATTR BLE_UpdateBAT()
{
    // 仅在 BLE 连接建立后更新电量特征值 (Battery Level Characteristic)
    if (keybrick.isConnected())
    {
        keybrick.setBatteryLevel(BAT_GetPercentage());
    }
}

// =================================================================================
// 系统初始化 (Setup)
// =================================================================================

void setup()
{

    KEY_Init();
    SYS_LoadPreset();             // 从 NVS 或存储加载用户配置
    SYS_ApplyPreset(currentPreset); // 应用当前配置

    // --- [新增] 开机立即读取电池电压 ---
    // 目的：避免 BAT_Read() 的时间间隔导致开机前几秒显示 00%
    
    // 1. 预读一次 ADC (让 ADC 电路稳定)
    analogRead(ADC_PIN);
    delay(20); 

    // 2. 正式读取
    uint16_t adcVal = analogRead(ADC_PIN);

    // 3. 计算电压 (复制 battery.cpp 中的标准公式)
    // 这里的 ADC_VREF 和 BAT_VOLT_DIVIDER 应该在 def.h 或 battery.h 中定义
    float adcVolt = (float)adcVal * ADC_VREF / 4096.0;
    BAT_Voltage = adcVolt / BAT_VOLT_DIVIDER;

    // 4. 初始化低电量标志
    if (BAT_Voltage < BAT_LOW) {
        BAT_IS_LOW = true;
    } else {
        BAT_IS_LOW = false;
    }
    // ------------------------------------

    OLED_Init(7, 6, 32, 0);
    pinMode(STATUS_LED, OUTPUT);
    pinMode(BUZZER_PIN, OUTPUT);

    /* 定时器 0: 用于按键扫描 (高频) */
    // 80分频 -> 80MHz / 80 = 1MHz (1us 计数一次)
    hw_timer_t *timer = timerBegin(0, 80, true);
    timerAttachInterrupt(timer, &KEY_Detect, true);
    timerAlarmWrite(timer, 10000, true); // 10000 * 1us = 10ms 触发一次
    timerAlarmEnable(timer);

    /* 定时器 1: 用于电池电量同步 (低频) */
    // 800分频 -> 80MHz / 800 = 100kHz (10us 计数一次)
    hw_timer_t *timer1 = timerBegin(1, 800, true);
    timerAttachInterrupt(timer1, &BLE_UpdateBAT, true);
    timerAlarmWrite(timer1, 6000000, true); // 6,000,000 * 10us = 60s (1min) 触发一次
    timerAlarmEnable(timer1);

    // 3. 启动 BLE 协议栈
    keybrick.begin();
}

// =================================================================================
// 主循环
// =================================================================================
void loop()
{

    // --- 1. 传感器与状态更新 ---
    active = KEY_Update(); // 获取按键物理层状态，返回系统是否活跃

    // 低电量强制低亮度保护
    if (BAT_IS_LOW)
    {
        OLED_LowBrightness(true);
    }

    SYS_ModeSwitch(); // 扫描是否触发了“模式切换”组合键

    // --- 2. BLE 连接管理 ---
    if (keybrick.isConnected())
    {
        sysStatus.bleConnected = true;
        // 仅在允许发送且已连接时处理 HID 发送
        if (enableKey)
        {
            KEY_Send();
        }
    }
    else
    {
        sysStatus.bleConnected = false;
        // TODO: 此处可添加自动重连逻辑 (Advertising Resume)
    }

    // --- 3. 模式状态机处理 (Mode Handling) ---
    switch (currentMode)
    {
    case MODE_NORMAL:
        // 状态切换保护：如果是刚进入此模式，清空屏幕并发送一次 Release 防止卡键
        if (!enableKey)
        {
            OLED_Clear();
            keybrick.send2Ble(release);
            scrollPos = 0;
        }
        enableKey = true; // 允许键盘发送功能
        TIMER_Display();  // 若定时器后台运行，显示倒计时
        break;

    case MODE_TIMER_SET:
        if (enableKey)
        {
            OLED_Clear();
            keybrick.send2Ble(release);
        }
        enableKey = false; // 此时按键用于调整时间，禁止发送 HID 键值
        TIMER_Set();       // 进入时间设置 UI 逻辑
        break;

    case MODE_METRONOME:
        if (enableKey)
        {
            OLED_Clear();
            keybrick.send2Ble(release);
        }
        enableKey = false;
        METRONOME_Set(); // 进入节拍器设置 UI 逻辑
        break;

    case MODE_KEY_CONFIG:
        if (enableKey)
        {
            OLED_Clear();
            keybrick.send2Ble(release);
            scrollPos = 0;
        }
        enableKey = false;
        SYS_KeyConfig(); // 进入按键预设配置 UI 逻辑
        break;
    }

    // --- 4. 后台任务与 UI 刷新 ---
    TIMER_Handle();      // 检查定时器到期事件 (蜂鸣器/LED)
    METRONOME_Handle();  // 处理节拍器声音输出
    BAT_Read();          // 读取 ADC 电压 (周期性更新)
    SYS_StatusLEDCtrl(); // 更新 LED 状态
    OLED_ChkTimeout();   // 检查屏幕超时
    OLED_Update();       // 刷新 OLED 显存到屏幕
}

// =================================================================================
// 辅助功能函数
// =================================================================================
/**
 * @brief  处理 HID 报文发送逻辑
 * @note   根据 keyState 状态构建并发送 BLE 报文
 */
void KEY_Send()
{
    // 遍历所有按键，检查是否有待发送的按下事件
    for (int i = 0; i < 5; i++)
    {
        if (keyState[i].shouldSend && !keyState[i].isReleased)
        {

            // 根据按键索引发送对应的键值缓冲区 (Key Buffer)
            switch (i)
            {
            case 0:
                keybrick.send2Ble(k1Buf);
                break;
            case 1:
                keybrick.send2Ble(k2Buf);
                break;
            case 2:
                keybrick.send2Ble(k3Buf);
                break;
            case 3:
                keybrick.send2Ble(k4Buf);
                break;
            case 4:
                keybrick.send2Ble(k5Buf);
                break;
            }

            // 发送后状态流转：等待释放，且清除“待发送”标志
            keyState[i].isReleased = true;
            keyState[i].shouldSend = false;
        }
    }

    // 处理全局释放事件 (All Keys Released)
    if (sendRelease)
    {
        keybrick.send2Ble(release); // 发送空报文，告诉主机按键已抬起
        sendRelease = false;
    }
}

/**
 * @brief  屏幕电源管理与超时检测
 * @note   包含三级状态：正常 -> 低亮度 -> 关闭
 */
void OLED_ChkTimeout()
{
    static uint32_t lastCheck = 0;

    // 1. 如果有活动 (按键操作)，重置计时器并唤醒屏幕
    if (active)
    {
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

    // 2. 每 1000ms 检查一次超时状态
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

/**
 * @brief  OLED 界面刷新函数
 * @note   根据 currentMode 渲染不同的 UI 元素
 */
void OLED_Update()
{

    if (!screenOn)
    {
        return;
    } // 屏幕关闭时不执行渲染，节省 I2C 总线资源

    switch (currentMode)
    {
    case MODE_NORMAL:
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
}

/**
 * @brief  定时器倒计时显示 (HUD)
 * @note   用于在 Normal 模式下叠加显示后台运行的定时器
 */
void TIMER_Display()
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