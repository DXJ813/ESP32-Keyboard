/**
  ******************************************************************************
  * @file    key.h
  * @brief   按键驱动头文件
  * @note    声明按键状态结构体、全局变量及对外接口
  ******************************************************************************
  */

#ifndef __KEY_H__
#define __KEY_H__

#include <Arduino.h>
#include "def.h"

#ifdef __cplusplus
extern "C" {
#endif

// =================================================================================
// 宏定义与结构体
// =================================================================================

// 长按判定阈值 (单位: ms)
#define LONG_PRESS_TIME 1500 

/** * @brief 按键状态结构体 
 */
typedef struct {
    bool isPressed;   // 当前物理状态：是否被按下
    bool shouldSend;  // 逻辑标志：是否应该发送 HID 报文
    bool isReleased;  // 逻辑标志：是否刚刚被释放 (用于触发 Release 报文)
} KeyState;

// =================================================================================
// 全局变量声明
// =================================================================================

// 全局按键使能标志 (true = 允许发送键值)
extern bool enableKey;

// --- 长按检测相关 (数组化) ---
// 索引 0-4 对应 Key1-Key5
extern bool keyLongPressed[5];       // 长按触发标志位
extern uint32_t keyPressStartTime[5]; // 按下起始时间戳

// 全局释放请求标志
extern bool sendRelease;

// --- HID 报文缓冲区 ---
// 对应 Key1 - Key5 的发送数据
extern char k1Buf[8];
extern char k2Buf[8];
extern char k3Buf[8];
extern char k4Buf[8];
extern char k5Buf[8];

// 通用释放报文 (全0)
extern char release[8];

// 按键状态数组
extern KeyState keyState[5];

// =================================================================================
// 函数原型
// =================================================================================

/**
  * @brief  初始化按键 GPIO
  */
void KEY_Init();

/**
  * @brief  更新按键状态 (去抖 + 长按检测)
  * @retval bool 是否有任意按键处于活动状态
  */
bool KEY_Update();

#ifdef __cplusplus
}
#endif

#endif
