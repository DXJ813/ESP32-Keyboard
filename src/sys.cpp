/******************************************************************************
 * @file    sys.cpp
 * @brief   系统核心逻辑与配置管理
 * @note    负责处理系统模式切换 (FSM)、HID 键值预设管理、NVS 参数存储以及状态指示。
 ******************************************************************************/

#include <Arduino.h> // 引入基础定义

// [关键修复] 显式以 C 语言方式引入 key.h
// 必须放在 #include "sys.h" 之前，确保编译器知道 KEY_Update 是 C 函数
#include "key.h"

#include "sys.h"
#include "ui_manager.h"
#include <Preferences.h> // ESP32 NVS (非易失性存储) 库

// 系统当前运行模式，默认为普通模式
SystemMode currentMode = MODE_NORMAL;

// 系统全局状态结构体初始化
struct SystemStatus sysStatus = {
    .bleConnected = false, // BLE 连接状态
    .lastLedUpdate = 0     // 上次 LED 状态翻转时间戳
};

/*
 * HID 报文修饰键 (Modifier Byte) 定义参考:
 * 0x01: Left Ctrl   0x02: Left Shift   0x04: Left Alt    0x08: Left GUI (Win/Cmd)
 * 0x10: Right Ctrl  0x20: Right Shift  0x40: Right Alt   0x80: Right GUI
 * * HID 报文结构 (8 Bytes):
 * Byte 0: 修饰键
 * Byte 1: 保留位 (Reserved)
 * Byte 2-7: 6个普通按键键值 (Keycodes)
 */

KeyPreset presets[PRESET_COUNT] = {

    // --- 预设 1:  图片 ---
    {
        {
            {0x01, 0x00, 0x1B, 0x00, 0x00, 0x00, 0x00, 0x00}, // Key 1: Ctrl+X (剪切)
            {0x01, 0x00, 0x19, 0x00, 0x00, 0x00, 0x00, 0x00}, // Key 2: Ctrl+V (粘贴)
            {0x00, 0x00, 0x4C, 0x00, 0x00, 0x00, 0x00, 0x00}, // Key 3: Delete (删除)
            {0x00, 0x00, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00}, // Key 4: Left Arrow  (左箭头)
            {0x00, 0x00, 0x4F, 0x00, 0x00, 0x00, 0x00, 0x00}  // Key 5: Right Arrow (右箭头)
        },
        "Image",                                     // 预设名称
        {"Cut", "Paste", "Delete", "~", "}"} // OLED 底部滚动的按键描述
    },

    // --- 预设 2 :  视频 ---
    {
        {
            {0x01, 0x00, 0x1B, 0x00, 0x00, 0x00, 0x00, 0x00}, // Key 1: Ctrl+X (剪切)
            {0x01, 0x00, 0x19, 0x00, 0x00, 0x00, 0x00, 0x00}, // Key 2: Ctrl+V (粘贴)
            {0x00, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x00}, // Key 3: Space  (空格/播放暂停)
            {0x00, 0x00, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00}, // Key 4: Left Arrow  (快退)
            {0x00, 0x00, 0x4F, 0x00, 0x00, 0x00, 0x00, 0x00}  // Key 5: Right Arrow (快进)
        },
        "Video",                                  // 预设名称
        {"Cut", "Paste", "Space", "~", "}"} // OLED 显示描述：~和}会显示为你改好的左右长箭头
    },
};

// 备用媒体键码表 (目前未使用)
char media[5][2]{
    {0x02, 0x00}, // Volume Up
    {0x03, 0x00}, // Volume Down
    {0x00, 0x10},
    {0x00, 0x80},
    {0x00, 0x00}};

// 全局控制变量
uint8_t currentPreset = 0; // 当前选中的预设索引
bool changeName = false;   // 预设切换标志 (用于触发 UI 刷新)

bool active = false;           // 系统活跃标志

/**
 * @brief  系统模式切换状态机 (FSM)
 * @note   根据按键的长按操作切换系统工作模式
 * @logic  逻辑优化：单键触发进入/退出
 * 长按 Key1 <-> 系统配置模式
 * 长按 Key2 <-> 定时器模式
 * 长按 Key3 <-> 节拍器模式
 */
void SYS_ModeSwitch()
{
    // --- 1. 检测 Key 1 长按 (系统配置模式) ---
    if (keyLongPressed[0])
    {
        // 如果当前已经在配置模式，则退出回正常模式
        if (currentMode == MODE_KEY_CONFIG)
        {
            currentMode = MODE_NORMAL;
        }
        // 否则进入配置模式
        else
        {
            currentMode = MODE_KEY_CONFIG;
        }

        // [BUG 修复] 等待 Key 1 物理释放
        // 必须等待手指抬起，否则因为 Key 1 同时也是 SYS_KeyConfig 的确认键，
        // 会导致刚进配置模式就立刻触发“确认退出”，造成闪退。
        while (keyState[0].isPressed) {
            KEY_Update(); // 现在这里可以正确链接到 C 函数了
            delay(10);
        }

        // 复位长按标志和计时器
        keyLongPressed[0] = false;
        keyPressStartTime[0] = 0;
    }

    // --- 2. 检测 Key 2 长按 (定时器模式) ---
    else if (keyLongPressed[1])
    {
        // 如果当前已经在定时器模式，则退出回正常模式
        if (currentMode == MODE_TIMER_SET)
        {
            currentMode = MODE_NORMAL;
        }
        // 否则进入定时器模式
        else
        {
            currentMode = MODE_TIMER_SET;
        }

        keyLongPressed[1] = false;
        keyPressStartTime[1] = 0;
    }

    // --- 3. 检测 Key 3 长按 (节拍器模式) ---
    else if (keyLongPressed[2])
    {
        // 如果当前已经在节拍器模式，则退出回正常模式
        if (currentMode == MODE_METRONOME)
        {
            currentMode = MODE_NORMAL;
        }
        // 否则进入节拍器模式
        else
        {
            currentMode = MODE_METRONOME;
        }

        keyLongPressed[2] = false;
        keyPressStartTime[2] = 0;
    }
}

/**
 * @brief  处理按键配置模式下的交互逻辑
 * @note   用于切换和选择不同的按键预设 (Preset)
 * @ui     Key 4: 上一个预设, Key 5: 下一个预设, Key 1: 确认并应用
 */
void SYS_KeyConfig()
{
    // Key 4: 切换到上一个预设
    // 对应 keyState[3]
    if (keyState[3].isPressed)
    {
        currentPreset = (currentPreset + PRESET_COUNT - 1) % PRESET_COUNT;
        UIManager::resetScroll();
        changeName = true; // 触发 UI 刷新名称
        delay(100);        // 简单防抖
    }

    // Key 5: 切换到下一个预设
    // 对应 keyState[4]
    if (keyState[4].isPressed)
    {
        currentPreset = (currentPreset + 1) % PRESET_COUNT;
        changeName = true;
        UIManager::resetScroll();
        delay(100);
    }

    // Key 1: 确认选择
    // 对应 keyState[0]
    if (keyState[0].isPressed)
    {
        SYS_ConfirmPreset(currentPreset); // 保存到 NVS
        SYS_ApplyPreset(currentPreset);   // 应用配置
        delay(500);
        currentMode = MODE_NORMAL; // 返回正常模式
    }
}

// 预留接口：保存自定义预设
void SYS_SavePreset()
{
}

/**
 * @brief  从 NVS 加载预设索引
 * @note   使用 Preferences 库读取掉电不丢失的配置
 */
void SYS_LoadPreset()
{
    Preferences prefs;
    prefs.begin("KEY_CONFIG", true);             // 打开命名空间，只读模式
    currentPreset = prefs.getUChar("preset", 0); // 读取 "preset" 键值，默认 0
    prefs.end();
}

/**
 * @brief  将当前预设索引写入 NVS
 * @param  preset 预设索引
 */
void SYS_ConfirmPreset(uint8_t preset)
{
    Preferences prefs;
    prefs.begin("KEY_CONFIG", false);        // 打开命名空间，读写模式
    prefs.putUChar("preset", currentPreset); // 写入数据
    prefs.end();
}

/**
 * @brief  应用指定的预设方案
 * @note   将 Flash 中的预设数据拷贝到活动的发送缓冲区 (kBuf)，并给予蜂鸣器反馈
 * @param  presetIndex 预设在数组中的索引
 */
void SYS_ApplyPreset(uint8_t presetIndex)
{
    if (presetIndex >= PRESET_COUNT)
    {
        return;
    } // 边界检查

    // 将预设数据 (ROM) 复制到 运行时缓冲区 (RAM)
    memcpy(k1Buf, presets[presetIndex].keymap[0], 8);
    memcpy(k2Buf, presets[presetIndex].keymap[1], 8);
    memcpy(k3Buf, presets[presetIndex].keymap[2], 8);
    memcpy(k4Buf, presets[presetIndex].keymap[3], 8);
    memcpy(k5Buf, presets[presetIndex].keymap[4], 8);

    // 成功提示音
    tone(BUZZER_PIN, 1000, 100);
    delay(100);
}

/**
 * @brief  系统状态 LED 控制逻辑
 * @note   根据 BLE 连接状态控制 LED 闪烁模式
 * - 已连接: 常亮
 * - 未连接: 1Hz 闪烁
 */
void SYS_StatusLEDCtrl()
{
    static bool ledState = LOW;

    // 如果需要在息屏时关闭 LED，可取消以下注释
    if (!UIManager::isScreenOn())
    {
        digitalWrite(STATUS_LED, LOW);
        return;
    }

    // 节拍器模式下不控制 LED，避免干扰节拍器可能的 LED 指示
    if (currentMode == MODE_METRONOME)
    {
        return;
    }

    if (sysStatus.bleConnected)
    {
        // BLE 已连接: LED 常亮
        digitalWrite(STATUS_LED, HIGH);
    }
    else
    {
        // BLE 未连接: 1Hz 频率闪烁 (500ms 翻转一次)
        if (millis() - sysStatus.lastLedUpdate > 500)
        {
            ledState = !ledState;
            digitalWrite(STATUS_LED, ledState);
            sysStatus.lastLedUpdate = millis();
        }
    }
}

// =================================================================================
// 从 main.cpp 迁移过来的逻辑
// =================================================================================

#include "timerMetronome.h"

/* 实例化 BLE HID 对象: 设备名, 制造商, 电量初始值 */
Hid2Ble keybrick("ESP32C3 BLE Keybrick", "dxj", 100);

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