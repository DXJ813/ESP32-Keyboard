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
#include "ui_manager.h"


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

    UIManager::begin();
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

    if (active) {
        UIManager::onActivity();
    }

    // 低电量强制低亮度保护
    if (BAT_IS_LOW)
    {
        UIManager::setLowBattery(true);
    }
    else 
    {
        UIManager::setLowBattery(false);
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
            UIManager::resetScroll();
        }
        enableKey = true; // 允许键盘发送功能
        // TIMER_Display();  // Managed by UIManager::update()
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
            UIManager::resetScroll(); 
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
    // OLED_ChkTimeout();   // Managed by UIManager
    UIManager::update();       // 刷新 OLED 显存到屏幕
}

