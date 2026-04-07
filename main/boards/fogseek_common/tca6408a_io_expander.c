#include "tca6408a_io_expander.h"
#include <esp_log.h>
#include <string.h>

static const char *TAG = "TCA6408A";

/**
 * @brief I2C 写入操作
 */
static esp_err_t tca6408a_i2c_write(tca6408a_handle_t *handle, uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t buffer[2];
    buffer[0] = reg;
    memcpy(&buffer[1], data, len);
    return i2c_master_transmit(handle->i2c_device, buffer, len + 1, 100);
}

/**
 * @brief I2C 读取操作
 */
static esp_err_t tca6408a_i2c_read(tca6408a_handle_t *handle, uint8_t reg, uint8_t *data, size_t len)
{
    // 先写入寄存器地址
    esp_err_t ret = i2c_master_transmit(handle->i2c_device, &reg, 1, 100);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to write register address: %d", ret);
        return ret;
    }
    
    // 读取数据
    ret = i2c_master_receive(handle->i2c_device, data, len, 100);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to read register: %d", ret);
        return ret;
    }
    
    return ESP_OK;
}

esp_err_t tca6408a_init(tca6408a_handle_t *handle, const tca6408a_config_t *config)
{
    if (handle == NULL || config == NULL)
    {
        ESP_LOGE(TAG, "Invalid arguments");
        return ESP_ERR_INVALID_ARG;
    }
    
    memset(handle, 0, sizeof(tca6408a_handle_t));
    handle->config = *config;
    handle->output_cache = 0xFF; // 默认所有输出为高电平
    handle->config_cache = 0xFF; // 默认所有 GPIO 为输入
    
    // 配置 I2C 设备信息
    i2c_device_config_t i2c_device_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = config->i2c_address,
        .scl_speed_hz = 400000, // 400kHz 快速模式
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = false,
        },
    };
    
    esp_err_t ret = i2c_master_bus_add_device(config->i2c_bus, &i2c_device_cfg, &handle->i2c_device);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to add I2C device: %d", ret);
        return ret;
    }
    
    // 检查设备是否存在
    if (!tca6408a_probe(handle))
    {
        ESP_LOGE(TAG, "TCA6408A not found at address 0x%02X", config->i2c_address);
        i2c_master_bus_remove_device(handle->i2c_device);
        return ESP_FAIL;
    }
    
    // 配置 RESET 引脚（如果提供）
    if (config->reset_gpio != GPIO_NUM_NC && config->reset_gpio >= 0)
    {
        gpio_config_t reset_conf = {
            .intr_type = GPIO_INTR_DISABLE,
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = (1ULL << config->reset_gpio),
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .pull_up_en = GPIO_PULLUP_DISABLE,
        };
        gpio_config(&reset_conf);
        
        // 产生复位脉冲（低电平有效）
        gpio_set_level(config->reset_gpio, 0);
        esp_rom_delay_us(10); // 保持低电平至少 10us
        gpio_set_level(config->reset_gpio, 1);
        esp_rom_delay_us(100); // 等待上电复位完成
        
        ESP_LOGI(TAG, "RESET pin configured on GPIO%d", config->reset_gpio);
    }
    
    // 配置 INT 引脚（如果提供）
    if (config->int_gpio != GPIO_NUM_NC && config->int_gpio >= 0)
    {
        gpio_config_t int_conf = {
            .intr_type = GPIO_INTR_POSEDGE, // 上升沿触发（INT 开漏，需要上拉）
            .mode = GPIO_MODE_INPUT,
            .pin_bit_mask = (1ULL << config->int_gpio),
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .pull_up_en = GPIO_PULLUP_ENABLE, // 启用内部上拉
        };
        gpio_config(&int_conf);
        
        ESP_LOGI(TAG, "INT pin configured on GPIO%d", config->int_gpio);
    }
    
    // 写入配置寄存器：设置 GPIO 方向
    // Edge V4.2: P4,P6,P7 为输入，其余为输出 => 0xE3 (1110 0011b)
    uint8_t config_data = 0xE3;
    ret = tca6408a_i2c_write(handle, TCA6408A_REG_CONFIG, &config_data, 1);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to write CONFIG register: %d", ret);
        return ret;
    }
    handle->config_cache = config_data;
    
    // 写入输出寄存器：初始所有输出为低电平
    uint8_t output_data = 0x00;
    ret = tca6408a_i2c_write(handle, TCA6408A_REG_OUTPUT, &output_data, 1);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to write OUTPUT register: %d", ret);
        return ret;
    }
    handle->output_cache = output_data;
    
    // 写入极性反转寄存器：默认不反转
    uint8_t polarity_data = 0x00;
    ret = tca6408a_i2c_write(handle, TCA6408A_REG_POLARITY, &polarity_data, 1);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to write POLARITY register: %d", ret);
        return ret;
    }
    
    handle->initialized = true;
    ESP_LOGI(TAG, "TCA6408A initialized successfully at 0x%02X", config->i2c_address);
    
    return ESP_OK;
}

esp_err_t tca6408a_deinit(tca6408a_handle_t *handle)
{
    if (handle == NULL || !handle->initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    
    // 释放 I2C 设备
    if (handle->i2c_device)
    {
        i2c_master_bus_remove_device(handle->i2c_device);
        handle->i2c_device = NULL;
    }
    
    // 释放 INT 引脚资源
    if (handle->config.int_gpio != GPIO_NUM_NC && handle->config.int_gpio >= 0)
    {
        gpio_reset_pin(handle->config.int_gpio);
    }
    
    // 释放 RESET 引脚资源
    if (handle->config.reset_gpio != GPIO_NUM_NC && handle->config.reset_gpio >= 0)
    {
        gpio_reset_pin(handle->config.reset_gpio);
    }
    
    handle->initialized = false;
    ESP_LOGI(TAG, "TCA6408A deinitialized");
    
    return ESP_OK;
}

esp_err_t tca6408a_set_gpio_direction(tca6408a_handle_t *handle, tca6408a_gpio_t gpio, tca6408a_direction_t direction)
{
    if (handle == NULL || !handle->initialized || gpio > TCA6408A_GPIO_P7)
    {
        return ESP_ERR_INVALID_ARG;
    }
    
    // 更新配置缓存
    if (direction == TCA6408A_DIR_INPUT)
    {
        handle->config_cache |= (1 << gpio); // 置 1 表示输入
    }
    else
    {
        handle->config_cache &= ~(1 << gpio); // 清 0 表示输出
    }
    
    // 写入配置寄存器
    return tca6408a_i2c_write(handle, TCA6408A_REG_CONFIG, &handle->config_cache, 1);
}

esp_err_t tca6408a_set_all_gpio_direction(tca6408a_handle_t *handle, uint8_t direction_mask)
{
    if (handle == NULL || !handle->initialized)
    {
        return ESP_ERR_INVALID_ARG;
    }
    
    handle->config_cache = direction_mask;
    return tca6408a_i2c_write(handle, TCA6408A_REG_CONFIG, &handle->config_cache, 1);
}

esp_err_t tca6408a_set_gpio_level(tca6408a_handle_t *handle, tca6408a_gpio_t gpio, uint8_t level)
{
    if (handle == NULL || !handle->initialized || gpio > TCA6408A_GPIO_P7)
    {
        return ESP_ERR_INVALID_ARG;
    }
    
    // 更新输出缓存
    if (level)
    {
        handle->output_cache |= (1 << gpio); // 置 1 表示高电平
    }
    else
    {
        handle->output_cache &= ~(1 << gpio); // 清 0 表示低电平
    }
    
    // 写入输出寄存器
    return tca6408a_i2c_write(handle, TCA6408A_REG_OUTPUT, &handle->output_cache, 1);
}

esp_err_t tca6408a_set_all_gpio_level(tca6408a_handle_t *handle, uint8_t level_mask)
{
    if (handle == NULL || !handle->initialized)
    {
        return ESP_ERR_INVALID_ARG;
    }
    
    handle->output_cache = level_mask;
    return tca6408a_i2c_write(handle, TCA6408A_REG_OUTPUT, &handle->output_cache, 1);
}

esp_err_t tca6408a_get_gpio_level(tca6408a_handle_t *handle, tca6408a_gpio_t gpio, uint8_t *level)
{
    if (handle == NULL || !handle->initialized || level == NULL || gpio > TCA6408A_GPIO_P7)
    {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t input_data;
    esp_err_t ret = tca6408a_i2c_read(handle, TCA6408A_REG_INPUT, &input_data, 1);
    if (ret != ESP_OK)
    {
        return ret;
    }
    
    *level = (input_data >> gpio) & 0x01;
    return ESP_OK;
}

esp_err_t tca6408a_get_all_gpio_level(tca6408a_handle_t *handle, uint8_t *level_mask)
{
    if (handle == NULL || !handle->initialized || level_mask == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    
    return tca6408a_i2c_read(handle, TCA6408A_REG_INPUT, level_mask, 1);
}

esp_err_t tca6408a_set_polarity(tca6408a_handle_t *handle, uint8_t polarity_mask)
{
    if (handle == NULL || !handle->initialized)
    {
        return ESP_ERR_INVALID_ARG;
    }
    
    return tca6408a_i2c_write(handle, TCA6408A_REG_POLARITY, &polarity_mask, 1);
}

esp_err_t tca6408a_reset(tca6408a_handle_t *handle)
{
    if (handle == NULL || !handle->initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (handle->config.reset_gpio == GPIO_NUM_NC || handle->config.reset_gpio < 0)
    {
        ESP_LOGW(TAG, "RESET pin not configured");
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    // 产生复位脉冲
    gpio_set_level(handle->config.reset_gpio, 0);
    esp_rom_delay_us(10);
    gpio_set_level(handle->config.reset_gpio, 1);
    esp_rom_delay_us(100);
    
    // 复位后重新初始化配置
    uint8_t config_data = 0xE3;
    esp_err_t ret = tca6408a_i2c_write(handle, TCA6408A_REG_CONFIG, &config_data, 1);
    if (ret != ESP_OK)
    {
        return ret;
    }
    handle->config_cache = config_data;
    
    uint8_t output_data = 0x00;
    ret = tca6408a_i2c_write(handle, TCA6408A_REG_OUTPUT, &output_data, 1);
    if (ret != ESP_OK)
    {
        return ret;
    }
    handle->output_cache = output_data;
    
    ESP_LOGI(TAG, "TCA6408A reset completed");
    return ESP_OK;
}

bool tca6408a_probe(tca6408a_handle_t *handle)
{
    if (handle == NULL || handle->i2c_device == NULL)
    {
        return false;
    }
    
    // 尝试读取输入寄存器来检测设备是否存在
    uint8_t dummy;
    esp_err_t ret = tca6408a_i2c_read(handle, TCA6408A_REG_INPUT, &dummy, 1);
    return (ret == ESP_OK);
}
