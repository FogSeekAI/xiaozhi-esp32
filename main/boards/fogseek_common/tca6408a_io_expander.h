#ifndef _TCA6408A_IO_EXPANDER_H_
#define _TCA6408A_IO_EXPANDER_H_

#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <esp_err.h>
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief TCA6408A 寄存器地址定义
 */
typedef enum
{
    TCA6408A_REG_INPUT = 0x00,      // 输入端口寄存器（只读）
    TCA6408A_REG_OUTPUT = 0x01,     // 输出端口寄存器（读写）
    TCA6408A_REG_POLARITY = 0x02,   // 极性反转寄存器（读写）
    TCA6408A_REG_CONFIG = 0x03      // 配置寄存器（读写）
} tca6408a_register_t;

/**
 * @brief TCA6408A GPIO 引脚定义（Edge V4.2）
 */
typedef enum
{
    TCA6408A_GPIO_P0 = 0,  // LCD 背光控制
    TCA6408A_GPIO_P1 = 1,  // 音频功放使能
    TCA6408A_GPIO_P2 = 2,  // 红色 LED
    TCA6408A_GPIO_P3 = 3,  // 绿色 LED
    TCA6408A_GPIO_P4 = 4,  // 控制按键（输入）
    TCA6408A_GPIO_P5 = 5,  // 电源保持
    TCA6408A_GPIO_P6 = 6,  // 充电完成检测（输入）
    TCA6408A_GPIO_P7 = 7   // 充电中检测（输入）
} tca6408a_gpio_t;

/**
 * @brief GPIO 方向配置
 */
typedef enum
{
    TCA6408A_DIR_OUTPUT = 0,  // 输出模式
    TCA6408A_DIR_INPUT = 1    // 输入模式
} tca6408a_direction_t;

/**
 * @brief TCA6408A 配置结构体
 */
typedef struct
{
    i2c_master_bus_handle_t i2c_bus;  // I2C 总线句柄
    uint8_t i2c_address;              // I2C 从机地址（7 位）
    gpio_num_t int_gpio;              // INT 中断引脚（可选）
    gpio_num_t reset_gpio;            // RESET 复位引脚（可选）
} tca6408a_config_t;

/**
 * @brief TCA6408A 驱动句柄
 */
typedef struct
{
    i2c_master_dev_handle_t i2c_device;  // I2C 设备句柄
    tca6408a_config_t config;            // 配置参数
    uint8_t output_cache;                // 输出缓存（减少 I2C 读取）
    uint8_t config_cache;                // 配置缓存（方向设置）
    bool initialized;                    // 初始化标志
} tca6408a_handle_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 TCA6408A IO 扩展器
 * 
 * @param handle 驱动句柄指针
 * @param config 配置参数
 * @return esp_err_t 错误代码
 * 
 * @note 初始化流程:
 * 1. 创建 I2C 设备
 * 2. 配置 RESET 引脚（如果提供）
 * 3. 配置 INT 引脚（如果提供）
 * 4. 写入初始配置到配置寄存器
 * 5. 写入初始输出状态
 */
esp_err_t tca6408a_init(tca6408a_handle_t *handle, const tca6408a_config_t *config);

/**
 * @brief 去初始化 TCA6408A
 * 
 * @param handle 驱动句柄指针
 * @return esp_err_t 错误代码
 */
esp_err_t tca6408a_deinit(tca6408a_handle_t *handle);

/**
 * @brief 配置单个 GPIO 引脚方向
 * 
 * @param handle 驱动句柄指针
 * @param gpio GPIO 引脚编号
 * @param direction 方向（输入/输出）
 * @return esp_err_t 错误代码
 */
esp_err_t tca6408a_set_gpio_direction(tca6408a_handle_t *handle, tca6408a_gpio_t gpio, tca6408a_direction_t direction);

/**
 * @brief 配置所有 GPIO 引脚方向
 * 
 * @param handle 驱动句柄指针
 * @param direction_mask 方向掩码（每位对应一个 GPIO，1=输入，0=输出）
 * @return esp_err_t 错误代码
 * 
 * @note 示例：0xE3 (1110 0011b) 表示 P4,P6,P7 为输入，其余为输出
 */
esp_err_t tca6408a_set_all_gpio_direction(tca6408a_handle_t *handle, uint8_t direction_mask);

/**
 * @brief 设置单个 GPIO 输出电平
 * 
 * @param handle 驱动句柄指针
 * @param gpio GPIO 引脚编号
 * @param level 电平值（0=低电平，1=高电平）
 * @return esp_err_t 错误代码
 * 
 * @note 仅对配置为输出的引脚有效
 */
esp_err_t tca6408a_set_gpio_level(tca6408a_handle_t *handle, tca6408a_gpio_t gpio, uint8_t level);

/**
 * @brief 设置所有 GPIO 输出电平
 * 
 * @param handle 驱动句柄指针
 * @param level_mask 电平掩码（每位对应一个 GPIO，1=高电平，0=低电平）
 * @return esp_err_t 错误代码
 * 
 * @note 仅对配置为输出的引脚有效
 */
esp_err_t tca6408a_set_all_gpio_level(tca6408a_handle_t *handle, uint8_t level_mask);

/**
 * @brief 读取单个 GPIO 输入电平
 * 
 * @param handle 驱动句柄指针
 * @param gpio GPIO 引脚编号
 * @param level 读取到的电平值（0=低电平，1=高电平）
 * @return esp_err_t 错误代码
 * 
 * @note 可读取任意引脚的实际电平（无论输入或输出模式）
 */
esp_err_t tca6408a_get_gpio_level(tca6408a_handle_t *handle, tca6408a_gpio_t gpio, uint8_t *level);

/**
 * @brief 读取所有 GPIO 输入电平
 * 
 * @param handle 驱动句柄指针
 * @param level_mask 读取到的电平掩码（每位对应一个 GPIO）
 * @return esp_err_t 错误代码
 * 
 * @note 返回所有引脚的实际输入电平
 */
esp_err_t tca6408a_get_all_gpio_level(tca6408a_handle_t *handle, uint8_t *level_mask);

/**
 * @brief 配置极性反转寄存器
 * 
 * @param handle 驱动句柄指针
 * @param polarity_mask 极性反转掩码（置 1 反转对应输入引脚逻辑）
 * @return esp_err_t 错误代码
 */
esp_err_t tca6408a_set_polarity(tca6408a_handle_t *handle, uint8_t polarity_mask);

/**
 * @brief 复位 TCA6408A（通过 RESET 引脚）
 * 
 * @param handle 驱动句柄指针
 * @return esp_err_t 错误代码
 * 
 * @note 需要配置了 reset_gpio 才有效
 */
esp_err_t tca6408a_reset(tca6408a_handle_t *handle);

/**
 * @brief 检查 TCA6408A 是否存在（I2C 通信测试）
 * 
 * @param handle 驱动句柄指针
 * @return bool true=存在，false=不存在
 */
bool tca6408a_probe(tca6408a_handle_t *handle);

#ifdef __cplusplus
}
#endif

#endif // _TCA6408A_IO_EXPANDER_H_
