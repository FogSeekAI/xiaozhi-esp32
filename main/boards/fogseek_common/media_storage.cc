#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "media_storage.h"

static const char *TAG = "media_storage";

#define MOUNT_POINT "/sdcard"
#define EXAMPLE_MAX_CHAR_SIZE 64

MediaStorage::MediaStorage()
    : is_initialized_(false), card_(nullptr)
{
    // 初始化配置结构体
    memset(&config_, 0, sizeof(config_));
}

MediaStorage::~MediaStorage()
{
    if (is_initialized_)
    {
        deinit();
    }
}

esp_err_t MediaStorage::Initialize(const media_storage_config_t *config)
{
    if (is_initialized_)
    {
        ESP_LOGW(TAG, "Media storage already initialized");
        return ESP_OK;
    }

    // 保存配置
    memcpy(&config_, config, sizeof(media_storage_config_t));

    esp_err_t ret;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,   // 如果挂载不成功是否需要格式化SD卡
        .max_files = 5,                   // 允许打开的最大文件数
        .allocation_unit_size = 16 * 1024 // 分配单元大小
    };

    const char mount_point[] = MOUNT_POINT;
    ESP_LOGI(TAG, "Initializing media storage device");
    ESP_LOGI(TAG, "Using SDMMC peripheral");

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();                      // SDMMC主机接口配置
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT(); // SDMMC插槽配置
    slot_config.width = 1;                                         // 设置为1线SD模式
    slot_config.clk = config_.clk;
    slot_config.cmd = config_.cmd;
    slot_config.d0 = config_.d0;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP; // 打开内部上拉电阻

    ESP_LOGI(TAG, "Mounting filesystem");
    ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card_); // 挂载SD卡

    if (ret != ESP_OK)
    { // 如果没有挂载成功
        if (ret == ESP_FAIL)
        { // 如果挂载失败
            ESP_LOGE(TAG, "Failed to mount filesystem. ");
        }
        else
        { // 如果是其它错误 打印错误名称
            ESP_LOGE(TAG, "Failed to initialize the card (%s). ", esp_err_to_name(ret));
        }
        // 在失败后等待1秒然后重启
        ESP_LOGI(TAG, "Waiting 1 second before restart...");
        vTaskDelay(1000 / portTICK_PERIOD_MS); // 延迟1秒
        esp_restart();                         // 重启ESP32
        return ret;
    }

    ESP_LOGI(TAG, "Filesystem mounted");  // 提示挂载成功
    sdmmc_card_print_info(stdout, card_); // 终端打印SD卡的一些信息

    is_initialized_ = true;
    return ESP_OK;
}

esp_err_t MediaStorage::deinit()
{
    if (!is_initialized_)
    {
        ESP_LOGW(TAG, "Media storage not initialized");
        return ESP_OK;
    }

    const char mount_point[] = MOUNT_POINT;
    esp_err_t ret = esp_vfs_fat_sdcard_unmount(mount_point, card_);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to unmount media storage device (%s)", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Media storage device unmounted");
    is_initialized_ = false;
    return ESP_OK;
}

esp_err_t MediaStorage::write_file_impl(const char *path, const char *data)
{
    char full_path[256];
    snprintf(full_path, sizeof(full_path), "%s%s", MOUNT_POINT, path);

    ESP_LOGI(TAG, "Opening file %s", full_path);
    FILE *f = fopen(full_path, "w"); // 以只写方式打开路径中文件
    if (f == NULL)
    {
        ESP_LOGE(TAG, "Failed to open file for writing");
        return ESP_FAIL;
    }
    fprintf(f, "%s", data); // 写入内容
    fclose(f);              // 关闭文件
    ESP_LOGI(TAG, "File written");

    return ESP_OK;
}

esp_err_t MediaStorage::read_file_impl(const char *path)
{
    char full_path[256];
    snprintf(full_path, sizeof(full_path), "%s%s", MOUNT_POINT, path);

    ESP_LOGI(TAG, "Reading file %s", full_path);
    FILE *f = fopen(full_path, "r"); // 以只读方式打开文件
    if (f == NULL)
    {
        ESP_LOGE(TAG, "Failed to open file for reading");
        return ESP_FAIL;
    }
    char line[EXAMPLE_MAX_CHAR_SIZE]; // 定义一个字符串数组
    fgets(line, sizeof(line), f);     // 获取文件中的内容到字符串数组
    fclose(f);                        // 关闭文件

    // strip newline
    char *pos = strchr(line, '\n'); // 查找字符串中的"\n"并返回其位置
    if (pos)
    {
        *pos = '\0'; // 把\n替换成\0
    }
    ESP_LOGI(TAG, "Read from file: '%s'", line); // 把数组内容输出到终端

    return ESP_OK;
}

esp_err_t MediaStorage::write_file(const char *path, const char *data)
{
    if (!is_initialized_)
    {
        ESP_LOGE(TAG, "Media storage not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    return write_file_impl(path, data);
}

esp_err_t MediaStorage::read_file(const char *path)
{
    if (!is_initialized_)
    {
        ESP_LOGE(TAG, "Media storage not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    return read_file_impl(path);
}

bool MediaStorage::file_exists(const char *path)
{
    char full_path[256];
    snprintf(full_path, sizeof(full_path), "%s%s", MOUNT_POINT, path);

    struct stat st;
    return (stat(full_path, &st) == 0);
}

esp_err_t MediaStorage::print_info()
{
    if (!is_initialized_)
    {
        ESP_LOGE(TAG, "Media storage not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    sdmmc_card_print_info(stdout, card_);
    return ESP_OK;
}