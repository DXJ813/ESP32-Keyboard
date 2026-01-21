/******************************************************************************
 * @file    sys.cpp
 * @brief   系统核心逻辑与配置管理
 * @note    负责处理系统模式切换 (FSM)、HID 键值预设管理、NVS 参数存储以及状态指示。
 ******************************************************************************/

#include <Arduino.h> // 引入基础定义

// [关键修复] 显式以 C 语言方式引入 key.h
// 必须放在 #include "sys.h" 之前，确保编译器知道 KEY_Update 是 C 函数
extern "C" {
    #include "key.h"
}

#include "sys.h"
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
uint8_t scrollPos = 0;     // OLED 滚动显示的当前位置
bool changeName = false;   // 预设切换标志 (用于触发 UI 刷新)

uint32_t lastActivityTime = 0; // 最后一次操作时间戳 (用于屏幕休眠)
bool screenOn = true;          // 屏幕电源状态
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
        scrollPos = 0;
        changeName = true; // 触发 UI 刷新名称
        delay(100);        // 简单防抖
    }

    // Key 5: 切换到下一个预设
    // 对应 keyState[4]
    if (keyState[4].isPressed)
    {
        currentPreset = (currentPreset + 1) % PRESET_COUNT;
        changeName = true;
        scrollPos = 0;
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
    if (!screenOn)
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