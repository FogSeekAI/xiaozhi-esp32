#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

// 按钮相关配置
#define BOOT_BUTTON_GPIO GPIO_NUM_0
#define CTRL_BUTTON_GPIO GPIO_NUM_18

// 电源相关配置
#define PWR_HOLD_GPIO GPIO_NUM_39
#define PWR_CHARGE_DONE_GPIO GPIO_NUM_21
#define PWR_CHARGING_GPIO GPIO_NUM_15
#define BATTERY_ADC_GPIO GPIO_NUM_10 // 电池电压检测ADC1_CH9

// LED相关配置
#define LED_RED_GPIO GPIO_NUM_48
#define LED_GREEN_GPIO GPIO_NUM_47

// 音频相关配置
#define AUDIO_INPUT_SAMPLE_RATE 24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000
#define AUDIO_INPUT_REFERENCE true

#define AUDIO_CODEC_I2C_SCL_PIN GPIO_NUM_11 // I2C时钟线
#define AUDIO_CODEC_I2C_SDA_PIN GPIO_NUM_12 // I2C数据线

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_9 // 主时钟
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_14 // 位时钟
#define AUDIO_I2S_GPIO_WS GPIO_NUM_16   // 帧时钟
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_13 // 数据输出（扬声器）
#define AUDIO_I2S_GPIO_DIN GPIO_NUM_17  // 数据输入（麦克风）

#define AUDIO_CODEC_PA_PIN GPIO_NUM_38                    // NS4150B功放使能引脚
#define AUDIO_CODEC_ES8389_ADDR ES8389_CODEC_DEFAULT_ADDR // ES8389默认I2C地址

// TCA6408A IO扩展器配置
#define TCA6408A_I2C_ADDRESS 0x20  // TCA6408A I2C地址 (ADDR接地)

#define I2C_INT_GPIO GPIO_NUM_5 // 中断
// TCA6408A引脚分配（使用枚举值，与tca6408a_io_expander.h保持一致）
#define TCA6408A_RADAR_PIN TCA6408A_GPIO_P3  // 雷达传感器连接到P3 (输入)
#define TCA6408A_MOTOR_PIN TCA6408A_GPIO_P6  // 电机控制连接到P6 (输出)
#define TCA6408A_TOUCH_PIN TCA6408A_GPIO_P7  // 触摸传感器连接到P7 (输入)

// 配置掩码：P3、P7为输入(1)，P6为输出(0)，其他保持输入(1)
// 二进制: 1 1 0 1 1 1 1 1 = 0xDF
#define TCA6408A_CONFIG_MASK 0x88







#endif