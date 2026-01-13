/******************************************************************************
 * @file    sys.cpp
 * @brief   系统核心逻辑与配置管理
 * @note    负责处理系统模式切换 (FSM)、HID 键值预设管理、NVS 参数存储以及状态指示。
 ******************************************************************************/

#include "sys.h"
#include <Preferences.h> // ESP32 NVS (非易失性存储) 库

// 系统当前运行模式，默认为普通模式
SystemMode currentMode = MODE_NORMAL;

// 系统全局状态结构体初始化
struct SystemStatus sysStatus = {
    .bleConnected = false, // BLE 连接状态
    .lastLedUpdate = 0 // 上次 LED 状态翻转时间戳
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
// Byte 0 (无修饰键), Byte 1 (保留), Byte 2 (Delete键码), 后面全0
//{ 0x00, 0x00, 0x4C, 0x00, 0x00, 0x00, 0x00, 0x00 }, // Delete
/* * 预设方案数组 (Presets)
 * 包含：5个按键的 HID 报文数据、预设名称、每个按键的功能描述字符串
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
        "Image", // 预设名称
        {"Cut", "Paste", "Delete", "~", "}"} // OLED 底部滚动的按键描述
    },

    // --- 预设 2 :  视频 ---
    {
        {
            {0x00}, // Key 1: 省略 (Empty)
            {0x00}, // Key 2: 省略 (Empty)
            {0x00, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x00}, // Key 3: Space (空格/播放暂停)
            {0x00, 0x00, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00}, // Key 4: Left Arrow  (快退)
            {0x00, 0x00, 0x4F, 0x00, 0x00, 0x00, 0x00, 0x00}  // Key 5: Right Arrow (快进)
        },
        "Video", // 预设名称
        {"N/A", "N/A", "Space", "~", "}"} // OLED 显示描述：~和}会显示为你改好的左右长箭头
    },
};

// 备用媒体键码表 (目前未使用)
char media[5][2]{
    {0x02, 0x00}, // Volume Up
    {0x03, 0x00}, // Volume Down
    {0x00, 0x10},
    {0x00, 0x80},
    {0x00, 0x00}
};

// 全局控制变量
uint8_t currentPreset = 0; // 当前选中的预设索引
uint8_t scrollPos = 0; // OLED 滚动显示的当前位置
bool changeName = false; // 预设切换标志 (用于触发 UI 刷新)

uint32_t lastActivityTime = 0; // 最后一次操作时间戳 (用于屏幕休眠)
bool screenOn = true; // 屏幕电源状态
bool active = false; // 系统活跃标志

/**
  * @brief  系统模式切换状态机 (FSM)
  * @note   根据按键的长按组合来切换系统的工作模式
  * @logic  长按 Key5 -> 定时器模式 / 返回正常模式
  * 长按 Key4 -> 节拍器模式
  * 同时长按 Key4 & Key5 -> 配置模式
  */
void SYS_ModeSwitch()
{
    // 检测 Key 5 长按 (进入/退出定时器模式)
    if (k5LongPressed && !keyState[3].isPressed)
    {
        if (currentMode == MODE_NORMAL)
        {
            currentMode = MODE_TIMER_SET;
        }
        else if ((currentMode == MODE_TIMER_SET) ||
            (currentMode == MODE_METRONOME) ||
            (currentMode == MODE_KEY_CONFIG))
        {
            currentMode = MODE_NORMAL; // 任意其他模式下长按 Key5 均返回正常模式
        }
        k5LongPressed = false; // 复位标志
        k5PressStartTime = 0;
    }
    // 检测 Key 4 长按 (进入节拍器模式)
    else if (k4LongPressed && !keyState[4].isPressed)
    {
        if (currentMode == MODE_NORMAL)
        {
            currentMode = MODE_METRONOME;
        };
        k4LongPressed = false;
        k4PressStartTime = 0;
    }
    // 检测 Key 4 和 Key 5 同时长按 (进入系统配置模式)
    if (k4LongPressed && k5LongPressed)
    {
        currentMode = MODE_KEY_CONFIG;
        k4LongPressed = false;
        k5LongPressed = false;
        k4PressStartTime = 0;
        k5PressStartTime = 0;
    }
}

/**
  * @brief  处理按键配置模式下的交互逻辑
  * @note   用于切换和选择不同的按键预设 (Preset)
  * @ui     Key 1: 上一个预设, Key 2: 下一个预设, Key 3: 确认并应用
  */
void SYS_KeyConfig()
{
    // Key 1: 切换到上一个预设 (循环)
    if (keyState[0].isPressed)
    {
        currentPreset = (currentPreset + PRESET_COUNT - 1) % PRESET_COUNT;
        scrollPos = 0;
        changeName = true; // 触发 UI 刷新名称
        delay(100); // 简单防抖
    }

    // Key 2: 切换到下一个预设 (循环)
    if (keyState[1].isPressed)
    {
        currentPreset = (currentPreset + 1) % PRESET_COUNT;
        changeName = true;
        scrollPos = 0;
        delay(100);
    }

    // Key 3: 确认选择
    if (keyState[2].isPressed)
    {
        SYS_ConfirmPreset(currentPreset); // 保存到 NVS
        SYS_ApplyPreset(currentPreset); // 应用配置
        delay(500);
        currentMode = MODE_NORMAL; // 返回正常模式
    }
}

// 预留接口：保存自定义预设 (目前为空)
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
    prefs.begin("KEY_CONFIG", true); // 打开命名空间，只读模式
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
    prefs.begin("KEY_CONFIG", false); // 打开命名空间，读写模式
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
    if (presetIndex >= PRESET_COUNT) { return; } // 边界检查

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
    // if(!screenOn) {
    //     digitalWrite(STATUS_LED, LOW);
    //     return;
    // }

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
