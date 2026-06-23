#ifndef MEDIA_STORAGE_H
#define MEDIA_STORAGE_H

#include <stdint.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/sdmmc_types.h"  // 修正包含路径

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 媒体存储设备配置结构体
 */
typedef struct {
    gpio_num_t clk;           /*!< SDMMC时钟引脚 */
    gpio_num_t cmd;           /*!< SDMMC命令引脚 */
    gpio_num_t d0;            /*!< SDMMC数据线0引脚 */
} media_storage_config_t;

#ifdef __cplusplus
}

/**
 * @brief 媒体存储类，用于管理SD卡等外部存储设备
 */
class MediaStorage {
public:
    /**
     * @brief 构造函数
     */
    MediaStorage();
    
    /**
     * @brief 析构函数
     */
    ~MediaStorage();
    
    /**
     * @brief 初始化媒体存储设备(SD卡)
     * 
     * @param config SD卡接口配置
     * @return esp_err_t - 成功返回ESP_OK，失败返回错误代码
     */
    esp_err_t Initialize(const media_storage_config_t *config);

    /**
     * @brief 卸载媒体存储设备(SD卡)
     * 
     * @return esp_err_t - 成功返回ESP_OK，失败返回错误代码
     */
    esp_err_t deinit();

    /**
     * @brief 写入文件到媒体存储设备
     * 
     * @param path 文件路径
     * @param data 要写入的数据
     * @return esp_err_t - 成功返回ESP_OK，失败返回错误代码
     */
    esp_err_t write_file(const char *path, const char *data);

    /**
     * @brief 从媒体存储设备读取文件
     * 
     * @param path 文件路径
     * @return esp_err_t - 成功返回ESP_OK，失败返回错误代码
     */
    esp_err_t read_file(const char *path);

    /**
     * @brief 检查文件是否存在
     * 
     * @param path 文件路径
     * @return true - 文件存在，false - 文件不存在
     */
    bool file_exists(const char *path);

    /**
     * @brief 获取媒体存储设备信息
     * 
     * @return esp_err_t - 成功返回ESP_OK，失败返回错误代码
     */
    esp_err_t print_info();

    /**
     * @brief 检查是否已初始化
     * 
     * @return true - 已初始化，false - 未初始化
     */
    bool is_initialized() const { return is_initialized_; }

private:
    bool is_initialized_;
    sdmmc_card_t *card_;
    media_storage_config_t config_;
    
    /**
     * @brief 写入文件实现
     * 
     * @param path 文件路径
     * @param data 要写入的数据
     * @return esp_err_t - 成功返回ESP_OK，失败返回错误代码
     */
    esp_err_t write_file_impl(const char *path, const char *data);

    /**
     * @brief 读取文件实现
     * 
     * @param path 文件路径
     * @return esp_err_t - 成功返回ESP_OK，失败返回错误代码
     */
    esp_err_t read_file_impl(const char *path);

    /**
     * @brief 电源使能GPIO初始化
     */
};

#endif // __cplusplus

#endif // MEDIA_STORAGE_H