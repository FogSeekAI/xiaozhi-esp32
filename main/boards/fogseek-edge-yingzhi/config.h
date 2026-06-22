#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

// 按钮相关配置
#define BOOT_BUTTON_GPIO GPIO_NUM_0
#define LED_SWITCH_GPIO GPIO_NUM_40   // IO40_LED: 单击打断对话，双击配网，长按开关RGB灯
#define NEXT_BUTTON_GPIO GPIO_NUM_47  // IO47_NEXT/V+: 单击下一首，长按增大音量
#define PREV_BUTTON_GPIO GPIO_NUM_39  // IO39_PREV/V-: 单击上一首，长按减小音量

// RGB LED 配置（直接使用 RgbLedStrip，不依赖 FogSeekLedController）
#define LED_RGB_GPIO GPIO_NUM_38       // IO38: RGB 灯带控制引脚
#define LED_RGB_NUM_LEDS 11            // RGB 灯珠数量

// 音频相关配置
#define AUDIO_INPUT_SAMPLE_RATE 24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000
#define AUDIO_INPUT_REFERENCE true

#define AUDIO_CODEC_I2C_SCL_PIN GPIO_NUM_1  // I2C时钟线
#define AUDIO_CODEC_I2C_SDA_PIN GPIO_NUM_2  // I2C数据线

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_18  // 主时钟
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_15  // 位时钟
#define AUDIO_I2S_GPIO_WS GPIO_NUM_16    // 帧时钟
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_17  // 数据输出（扬声器）
#define AUDIO_I2S_GPIO_DIN GPIO_NUM_21   // 数据输入（麦克风）

#define AUDIO_CODEC_PA_PIN GPIO_NUM_NC  // NS4150B功放使能引脚
#define AUDIO_CODEC_ES8311_ADDR ES8311_CODEC_DEFAULT_ADDR
#define AUDIO_CODEC_ES7210_ADDR ES7210_CODEC_DEFAULT_ADDR

// UART串口配置（用于 AC7065E 蓝牙音频协处理器通信）
#define AC7065E_UART_TX_PIN GPIO_NUM_43  // UART TX 引脚
#define AC7065E_UART_RX_PIN GPIO_NUM_44  // UART RX 引脚
#define AC7065E_UART_BAUD_RATE 115200    // 波特率

#endif