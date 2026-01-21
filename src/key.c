/**
  ******************************************************************************
  * @file    key.cpp
  * @brief   按键驱动及状态机处理
  * @note    包含按键去抖、状态更新及所有按键的长按检测逻辑
  ******************************************************************************
  */

#include "key.h"

// 宏定义：读取按键物理电平 (假设低电平有效)
#define pressed(key) !digitalRead(key)

// 全局使能标志
bool enableKey = true;

// 按键引脚映射数组 (便于循环处理)
const uint8_t KEY_PINS[5] = {BTN_1_PIN, BTN_2_PIN, BTN_3_PIN, BTN_4_PIN, BTN_5_PIN};

// 按键状态结构体数组
KeyState keyState[5] = {
    { false, false, false },
    { false, false, false },
    { false, false, false },
    { false, false, false },
    { false, false, false }
};

// 标志位：对应按键是否触发了长按
bool keyLongPressed[5] = {false, false, false, false, false};
// 计时器：记录按下时的系统时间
uint32_t keyPressStartTime[5] = {0, 0, 0, 0, 0};

// 全局标志：请求发送释放报文
bool sendRelease = false;

// --- HID 报文缓冲区定义 ---
// 格式: [修饰键, 保留, 键码1, 键码2, 键码3, 键码4, 键码5, 键码6]
char k1Buf[8] = { 0x01, 0x00, 0x1B, 0x00, 0x00, 0x00, 0x00, 0x00 }; // Ctrl + X (剪切)
char k2Buf[8] = { 0x01, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00 }; // Ctrl + C (复制)
char k3Buf[8] = { 0x01, 0x00, 0x19, 0x00, 0x00, 0x00, 0x00, 0x00 }; // Ctrl + V (粘贴)
char k4Buf[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }; // 预留
char k5Buf[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }; // 预留

// 空报文 (释放所有按键)
char release[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }; 

/**
  * @brief  初始化按键 GPIO
  * @param  None
  * @retval None
  */
void KEY_Init() {
    for (int i = 0; i < 5; i++) {
        pinMode(KEY_PINS[i], INPUT);
    }
}

/**
  * @brief  检测按键状态 (轮询模式)
  * @note   包含去抖动逻辑 (5ms) 和长按时间判定
  * @param  None
  * @retval bool 是否有任意按键处于按下状态
  */
bool KEY_Update() {
    bool isAnyKeyPressed = false;

    // 循环处理 5 个按键
    for (int i = 0; i < 5; i++) {
        
        // --- 1. 物理按键去抖动处理 ---
        if (pressed(KEY_PINS[i])) {
            delay(5); // 简单的阻塞式消抖 (注意：多键同时按可能会叠加延时，但在宏键盘应用中可接受)
            if (pressed(KEY_PINS[i])) {
                isAnyKeyPressed = true;
                keyState[i].isPressed = true;
            } else {
                // 可能是干扰信号
                keyState[i].isPressed = false; 
            }
        } else {
            keyState[i].isPressed = false;
        }

        // --- 2. 长按逻辑检测  ---
        if (keyState[i].isPressed) {
            // 如果是刚按下，记录起始时间
            if (keyPressStartTime[i] == 0) {
                keyPressStartTime[i] = millis();
            } 
            // 如果按下时间超过阈值 (LONG_PRESS_TIME 需在头文件定义)
            else if (millis() - keyPressStartTime[i] > LONG_PRESS_TIME) {
                keyLongPressed[i] = true; // 置位长按标志
                keyPressStartTime[i] = 0; // 重置计时器，防止重复触发 (实现单次长按触发)
                
                // 如果需要长按连发功能，这里可以不重置为0，而是重置为 millis() - (LONG_PRESS_TIME - REPEAT_INTERVAL)
            }
        } else {
            // 按键抬起，重置计时器
            keyPressStartTime[i] = 0;
            // 注意：keyLongPressed[i] 标志位通常需要在消费端(sys.cpp)处理完逻辑后手动清除
        }
    }

    return isAnyKeyPressed;
}