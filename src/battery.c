/**
 ******************************************************************************
 * @file    battery.c
 * @brief   电池电源管理模块
 * @note    包含 ADC 采样、电压计算及基于查表法 (LUT) 的电量百分比估算
 ******************************************************************************
 */

#include <Arduino.h> // 引入 Arduino 核心库以使用 analogRead, millis 等
#include "battery.h"

// =================================================================================
// 全局变量定义
// =================================================================================

// 电池低电量标志位 (True = 低电量警告)
bool BAT_IS_LOW = false;

// 当前电池端电压 (单位: V)
float BAT_Voltage = 0.0;

// =================================================================================
// 锂电池放电曲线查找表 (Lookup Table)
// =================================================================================
// 格式: {电压(V), 百分比(%)}
// 注意: 必须按电压从大到小排列
// 数据源参考: 通用 3.7V 锂聚合物电池放电曲线
typedef struct
{
  float voltage;
  uint8_t percentage;
} BatLutNode;

const BatLutNode Lipoly_LUT[] = {
    {4.15, 100}, // 满电 (4.2V 实际上很快会降到 4.15V)
    {4.05, 95},
    {3.97, 90},
    {3.90, 80},
    {3.80, 70},
    {3.73, 60},
    {3.67, 50}, // 3.7V 左右是核心平台期
    {3.61, 40},
    {3.56, 30},
    {3.50, 20}, // 3.5V 以下电压开始跳水
    {3.42, 10},
    {3.35, 5}, // 严重低电量
    {3.25, 0}  // 保护板截止电压附近
};

#define LUT_SIZE (sizeof(Lipoly_LUT) / sizeof(Lipoly_LUT[0]))

// =================================================================================
// 功能函数实现
// =================================================================================

/**
 * @brief  读取 ADC 并计算实际电池电压
 * @note   包含采样间隔控制，避免频繁操作 ADC 占用 CPU
 * @param  None
 * @retval None
 */
void BAT_Read()
{
  static uint32_t batReadStartTime = 0;

  // 首次运行初始化时间戳
  if (batReadStartTime == 0)
  {
    batReadStartTime = millis();
  }
  // 检查是否达到读取间隔 (BAT_READ_TIME_GAP 定义在 battery.h)
  else if (millis() - batReadStartTime > BAT_READ_TIME_GAP)
  {

    // 1. 读取 ADC 原始数值 (ESP32-C3 默认为 12-bit: 0~4095)
    uint16_t adcValue = analogRead(ADC_PIN);

    // 2. 转换为 ADC 引脚电压
    // 公式: ADC值 * 参考电压 / 分辨率
    float adcVoltage = (float)adcValue * ADC_VREF / 4096.0;

    // 3. 计算电池实际电压 (补偿硬件分压电路)
    BAT_Voltage = adcVoltage / BAT_VOLT_DIVIDER;

    // 4. 更新低电量警告标志
    // 这里依然使用宏定义 BAT_LOW (推荐设为 3.40V 左右)
    (BAT_Voltage < BAT_LOW) ? (BAT_IS_LOW = true) : (BAT_IS_LOW = false);

    // 重置计时器
    batReadStartTime = 0;
  }
}

/**
 * @brief  获取电池电量百分比
 * @note   [新算法] 使用查表法 + 线性插值，解决锂电池非线性放电问题
 * @param  None
 * @retval uint8_t (0-100)
 */
uint8_t BAT_GetPercentage()
{

  if (BAT_Voltage >= Lipoly_LUT[0].voltage)
    return 100;
  if (BAT_Voltage <= Lipoly_LUT[LUT_SIZE - 1].voltage)
    return 0;

  for (int i = 0; i < LUT_SIZE - 1; i++)
  {
    // 如果当前电压处于 Lut[i] 和 Lut[i+1] 之间
    if (BAT_Voltage <= Lipoly_LUT[i].voltage && BAT_Voltage > Lipoly_LUT[i + 1].voltage)
    {

      // 获取区间两端的电压和百分比
      float vHigh = Lipoly_LUT[i].voltage;
      float vLow = Lipoly_LUT[i + 1].voltage;
      uint8_t pHigh = Lipoly_LUT[i].percentage;
      uint8_t pLow = Lipoly_LUT[i + 1].percentage;

      // 公式: 当前百分比 = 低位百分比 + (当前电压 - 低位电压) / (高位电压 - 低位电压) * (百分比差值)
      float percentage = pLow + (BAT_Voltage - vLow) / (vHigh - vLow) * (pHigh - pLow);

      return (uint8_t)percentage;
    }
  }

  return 0;
  // -------------------------------------------------------------------------
  // 旧版代码: 简单线性映射 (Linear Mapping) - 已弃用/备份
  // -------------------------------------------------------------------------
  /*
  // 防止除零错误或负数溢出
  if (BAT_Voltage <= BAT_EMPTY) return 0;

  // 线性计算: (当前 - 最低) / (最高 - 最低)
  float percentage = (BAT_Voltage - BAT_EMPTY) / (BAT_FULL - BAT_EMPTY) * 100.0;

  // 结果限幅
  uint8_t batPercentage = (uint8_t)percentage;
  batPercentage = (batPercentage > 100) ? 100 : batPercentage;

  return batPercentage;
  */
}