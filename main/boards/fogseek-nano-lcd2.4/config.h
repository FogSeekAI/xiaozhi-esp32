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

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_38 // 主时钟
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_14 // 位时钟
#define AUDIO_I2S_GPIO_WS GPIO_NUM_16   // 帧时钟
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_13 // 数据输出（扬声器）
#define AUDIO_I2S_GPIO_DIN GPIO_NUM_17  // 数据输入（麦克风）

#define AUDIO_CODEC_PA_PIN GPIO_NUM_41                    // NS4150B功放使能引脚
#define AUDIO_CODEC_ES8389_ADDR ES8389_CODEC_DEFAULT_ADDR // ES8389默认I2C地址

// LCD 屏幕相关配置
#define BOARD_LCD_TYPE DISPLAY_TYPE_NANO_2_4_INCH // 2.4寸TFT LCD (ST7789, 240x320)

// SPI通信接口引脚定义 (4-Line SPI)
#define DISPLAY_SPI_MOSI_GPIO GPIO_NUM_7  // SDA - SPI数据线
#define DISPLAY_SPI_SCLK_GPIO GPIO_NUM_6  // SCL - SPI时钟线
#define DISPLAY_SPI_CS_GPIO GPIO_NUM_5    // CSX - 片选，低电平使能
#define DISPLAY_SPI_DC_GPIO GPIO_NUM_4    // D/C - 数据/命令选择
#define DISPLAY_RESET_GPIO GPIO_NUM_8     // RESX - 复位，低电平有效
#define DISPLAY_TE_GPIO GPIO_NUM_40       // TE - 撕裂效应输出（可选）
#define DISPLAY_BL_GPIO GPIO_NUM_NC       // 背光由硬件电路控制，无需软件操作

// 通用面板特性配置
#define DISPLAY_WIDTH 240
#define DISPLAY_HEIGHT 320
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY false
#define DISPLAY_OFFSET_X 0
#define DISPLAY_OFFSET_Y 0
#define DISPLAY_ROTATION 0

#endif
