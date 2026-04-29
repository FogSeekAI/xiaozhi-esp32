/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>
#include <sys/cdefs.h>
#include "sdkconfig.h"
#if CONFIG_LCD_ENABLE_DEBUG_LOG
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#endif
#include "esp_lcd_panel_io_interface.h"
#include "esp_lcd_panel_io.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_private/gpio.h"
#include "hal/gpio_ll.h"
#include "esp_log.h"
#include "esp_check.h"
#include "tca6408a_io_expander.h"
#include "esp_lcd_panel_io_spi_expander.h"

static const char *TAG = "lcd_panel.io.spi_expander";

typedef struct {
    spi_transaction_t base;
    struct {
        unsigned int dc_gpio_level: 1;
        unsigned int en_trans_done_cb: 1;
        unsigned int cs_level: 1;
    } flags;
} lcd_spi_trans_descriptor_t;

typedef struct {
    esp_lcd_panel_io_t base;
    spi_device_handle_t spi_dev;
    size_t spi_trans_max_bytes;
    int dc_gpio_num;
    tca6408a_handle_t *expander_handle;
    uint8_t cs_expander_pin;
    uint8_t bl_expander_pin;
    bool bl_use_expander;
    esp_lcd_panel_io_color_trans_done_cb_t on_color_trans_done;
    void *user_ctx;
    size_t queue_size;
    size_t num_trans_inflight;
    int lcd_cmd_bits;
    int lcd_param_bits;
    uint8_t cs_ena_pretrans;
    uint8_t cs_ena_posttrans;
    struct {
        unsigned int dc_cmd_level: 1;
        unsigned int dc_data_level: 1;
        unsigned int dc_param_level: 1;
        unsigned int octal_mode: 1;
        unsigned int quad_mode: 1;
    } flags;
    lcd_spi_trans_descriptor_t trans_pool[];
} esp_lcd_panel_io_spi_expander_t;

static esp_err_t panel_io_spi_expander_tx_param(esp_lcd_panel_io_t *io, int lcd_cmd, const void *param, size_t param_size);
static esp_err_t panel_io_spi_expander_tx_color(esp_lcd_panel_io_t *io, int lcd_cmd, const void *color, size_t color_size);
static esp_err_t panel_io_spi_expander_del(esp_lcd_panel_io_t *io);
static void lcd_spi_expander_pre_trans_cb(spi_transaction_t *trans);
static void lcd_spi_expander_post_trans_color_cb(spi_transaction_t *trans);
static esp_err_t expander_set_cs(tca6408a_handle_t *handle, uint8_t cs_pin, bool level);
static esp_err_t expander_set_bl(tca6408a_handle_t *handle, uint8_t bl_pin, bool level);
static esp_err_t panel_io_spi_expander_register_event_callbacks(esp_lcd_panel_io_handle_t io, 
                                                                 const esp_lcd_panel_io_callbacks_t *cbs, 
                                                                 void *user_ctx);
static esp_err_t expander_set_cs(tca6408a_handle_t *handle, uint8_t cs_pin, bool level) {
    if (!handle) return ESP_ERR_INVALID_ARG;
    return tca6408a_set_gpio_level(handle, (tca6408a_gpio_t)cs_pin, level ? 0 : 1);
}

static esp_err_t expander_set_bl(tca6408a_handle_t *handle, uint8_t bl_pin, bool level) {
    if (!handle) return ESP_ERR_INVALID_ARG;
    return tca6408a_set_gpio_level(handle, (tca6408a_gpio_t)bl_pin, level ? 1 : 0);
}

esp_err_t esp_lcd_new_panel_io_spi_expander(spi_host_device_t spi_host, 
                                            const esp_lcd_panel_io_spi_expander_config_t *io_config,
                                            esp_lcd_panel_io_handle_t *ret_io)
{
    size_t max_trans_bytes = 0;
    esp_err_t ret = ESP_OK;
    esp_lcd_panel_io_spi_expander_t *spi_panel_io = NULL;
    
    if (!io_config || !ret_io) {
        ESP_LOGE(TAG, "invalid argument: io_config=%p, ret_io=%p", io_config, ret_io);
        return ESP_ERR_INVALID_ARG;
    }
    if (io_config->expander_handle == NULL) {
        ESP_LOGE(TAG, "expander handle is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    spi_panel_io = (esp_lcd_panel_io_spi_expander_t*)calloc(1, sizeof(esp_lcd_panel_io_spi_expander_t) + 
                         sizeof(lcd_spi_trans_descriptor_t) * io_config->trans_queue_depth);
    if (!spi_panel_io) {
        ESP_LOGE(TAG, "no mem for spi panel io");
        return ESP_ERR_NO_MEM;
    }

    // 不使用结构体初始化器，逐个字段赋值以避免顺序问题
    spi_device_interface_config_t dev_cfg;
    memset(&dev_cfg, 0, sizeof(dev_cfg));
    
    dev_cfg.command_bits = 0;
    dev_cfg.address_bits = 0;
    dev_cfg.dummy_bits = 0;
    dev_cfg.mode = io_config->spi_mode;
    dev_cfg.clock_speed_hz = io_config->pclk_hz;
    dev_cfg.spics_io_num = -1;
    dev_cfg.queue_size = io_config->trans_queue_depth;
    dev_cfg.pre_cb = lcd_spi_expander_pre_trans_cb;
    dev_cfg.post_cb = lcd_spi_expander_post_trans_color_cb;
    dev_cfg.cs_ena_pretrans = io_config->cs_ena_pretrans;
    dev_cfg.cs_ena_posttrans = io_config->cs_ena_posttrans;
    
    // 设置 flags
    dev_cfg.flags = SPI_DEVICE_HALFDUPLEX;
    if (io_config->flags.lsb_first) dev_cfg.flags |= SPI_DEVICE_TXBIT_LSBFIRST;
    if (io_config->flags.sio_mode) dev_cfg.flags |= SPI_DEVICE_3WIRE;
    
    ret = spi_bus_add_device(spi_host, &dev_cfg, &spi_panel_io->spi_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "adding spi device to bus failed: %s", esp_err_to_name(ret));
        goto err;
    }

    // 配置 DC 引脚
    if (io_config->dc_gpio_num >= 0) {
        gpio_set_level((gpio_num_t)io_config->dc_gpio_num, 0);
        gpio_func_sel((gpio_num_t)io_config->dc_gpio_num, PIN_FUNC_GPIO);
        gpio_output_enable((gpio_num_t)io_config->dc_gpio_num);
    }

    // 保存配置
    spi_panel_io->flags.dc_cmd_level = io_config->flags.dc_high_on_cmd;
    spi_panel_io->flags.dc_data_level = !io_config->flags.dc_low_on_data;
    spi_panel_io->flags.dc_param_level = !io_config->flags.dc_low_on_param;
    spi_panel_io->flags.octal_mode = io_config->flags.octal_mode;
    spi_panel_io->flags.quad_mode = io_config->flags.quad_mode;
    spi_panel_io->on_color_trans_done = io_config->on_color_trans_done;
    spi_panel_io->user_ctx = io_config->user_ctx;
    spi_panel_io->lcd_cmd_bits = io_config->lcd_cmd_bits;
    spi_panel_io->lcd_param_bits = io_config->lcd_param_bits;
    spi_panel_io->dc_gpio_num = io_config->dc_gpio_num;
    spi_panel_io->queue_size = io_config->trans_queue_depth;
    spi_panel_io->expander_handle = io_config->expander_handle;
    spi_panel_io->cs_expander_pin = io_config->cs_expander_pin;
    spi_panel_io->bl_expander_pin = io_config->bl_pin;
    spi_panel_io->bl_use_expander = io_config->bl_use_expander;
    spi_panel_io->cs_ena_pretrans = io_config->cs_ena_pretrans;
    spi_panel_io->cs_ena_posttrans = io_config->cs_ena_posttrans;
    
    // 初始化扩展器引脚状态
    expander_set_cs(spi_panel_io->expander_handle, spi_panel_io->cs_expander_pin, false);
    
    if (spi_panel_io->bl_use_expander) {
        expander_set_bl(spi_panel_io->expander_handle, spi_panel_io->bl_expander_pin, true);
    }

    // 获取最大传输长度
    ret = spi_bus_get_max_transaction_len(spi_host, &max_trans_bytes);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "get spi max transaction len failed: %s", esp_err_to_name(ret));
        goto err;
    }
    spi_panel_io->spi_trans_max_bytes = max_trans_bytes;

    // 设置回调函数
    spi_panel_io->base.tx_param = panel_io_spi_expander_tx_param;
    spi_panel_io->base.tx_color = panel_io_spi_expander_tx_color;
    spi_panel_io->base.del = panel_io_spi_expander_del;
    spi_panel_io->base.register_event_callbacks = panel_io_spi_expander_register_event_callbacks;  // 加这一行！
    
    *ret_io = &(spi_panel_io->base);
    ESP_LOGI(TAG, "new spi lcd panel io expander @%p, max_trans_bytes=%d, cs_pin=%d, bl_pin=%d", 
             spi_panel_io, (int)max_trans_bytes, io_config->cs_expander_pin, io_config->bl_pin);

    return ESP_OK;

    // 在设置完所有回调后，验证
    ESP_LOGI(TAG, "=== Validating panel_io callbacks ===");
    ESP_LOGI(TAG, "  tx_param = %p", spi_panel_io->base.tx_param);
    ESP_LOGI(TAG, "  tx_color = %p", spi_panel_io->base.tx_color);
    ESP_LOGI(TAG, "  del = %p", spi_panel_io->base.del);
    
    if (spi_panel_io->base.tx_param == NULL || 
        spi_panel_io->base.tx_color == NULL || 
        spi_panel_io->base.del == NULL) {
        ESP_LOGE(TAG, "ERROR: Some callbacks are NULL!");
        goto err;
    }
    
err:
    if (spi_panel_io) {
        if (io_config->dc_gpio_num >= 0) {
            gpio_output_disable((gpio_num_t)io_config->dc_gpio_num);
        }
        free(spi_panel_io);
    }
    return ret;
}

static esp_err_t panel_io_spi_expander_del(esp_lcd_panel_io_t *io)
{
    esp_err_t ret = ESP_OK;
    spi_transaction_t *spi_trans = NULL;
    esp_lcd_panel_io_spi_expander_t *spi_panel_io = __containerof(io, esp_lcd_panel_io_spi_expander_t, base);

    size_t num_trans_inflight = spi_panel_io->num_trans_inflight;
    for (size_t i = 0; i < num_trans_inflight; i++) {
        ret = spi_device_get_trans_result(spi_panel_io->spi_dev, &spi_trans, portMAX_DELAY);
        if (ret != ESP_OK) break;
        spi_panel_io->num_trans_inflight--;
    }
    
    spi_bus_remove_device(spi_panel_io->spi_dev);
    
    if (spi_panel_io->dc_gpio_num >= 0) {
        gpio_output_disable((gpio_num_t)spi_panel_io->dc_gpio_num);
    }
    
    ESP_LOGD(TAG, "del lcd panel io spi expander @%p", spi_panel_io);
    free(spi_panel_io);

    return ret;
}

static void spi_lcd_expander_prepare_cmd_buffer(esp_lcd_panel_io_spi_expander_t *panel_io, const void *cmd)
{
    uint8_t *from = (uint8_t *)cmd;
    if (panel_io->lcd_cmd_bits > 8) {
        int start = 0;
        int end = panel_io->lcd_cmd_bits / 8 - 1;
        while (start < end) {
            uint8_t tmp = from[start];
            from[start] = from[end];
            from[end] = tmp;
            start++;
            end--;
        }
    }
}

static void spi_lcd_expander_prepare_param_buffer(esp_lcd_panel_io_spi_expander_t *panel_io, const void *param, size_t param_size)
{
    uint8_t *from = (uint8_t *)param;
    int param_width = panel_io->lcd_param_bits / 8;
    size_t param_num = param_size / param_width;
    if (panel_io->lcd_param_bits > 8) {
        for (size_t i = 0; i < param_num; i++) {
            int start = i * param_width;
            int end = start + param_width - 1;
            while (start < end) {
                uint8_t tmp = from[start];
                from[start] = from[end];
                from[end] = tmp;
                start++;
                end--;
            }
        }
    }
}

static esp_err_t panel_io_spi_expander_tx_param(esp_lcd_panel_io_t *io, int lcd_cmd, const void *param, size_t param_size)
{
    esp_err_t ret = ESP_OK;
    spi_transaction_t *spi_trans = NULL;
    lcd_spi_trans_descriptor_t *lcd_trans = NULL;
    esp_lcd_panel_io_spi_expander_t *spi_panel_io = __containerof(io, esp_lcd_panel_io_spi_expander_t, base);
    bool send_cmd = (lcd_cmd >= 0);

    ret = spi_device_acquire_bus(spi_panel_io->spi_dev, portMAX_DELAY);
    if (ret != ESP_OK) return ret;

    size_t num_trans_inflight = spi_panel_io->num_trans_inflight;
    for (size_t i = 0; i < num_trans_inflight; i++) {
        ret = spi_device_get_trans_result(spi_panel_io->spi_dev, &spi_trans, portMAX_DELAY);
        if (ret != ESP_OK) goto err;
        spi_panel_io->num_trans_inflight--;
    }
    
    lcd_trans = &spi_panel_io->trans_pool[0];
    memset(lcd_trans, 0, sizeof(lcd_spi_trans_descriptor_t));

    lcd_trans->base.user = (void*)spi_panel_io;
    
    expander_set_cs(spi_panel_io->expander_handle, spi_panel_io->cs_expander_pin, true);
    lcd_trans->flags.cs_level = 1;
    
    if (param && param_size) {
        lcd_trans->base.flags |= SPI_TRANS_CS_KEEP_ACTIVE;
    }

    if (send_cmd) {
        uint8_t cmd_buffer[4];
        memcpy(cmd_buffer, &lcd_cmd, sizeof(lcd_cmd));
        spi_lcd_expander_prepare_cmd_buffer(spi_panel_io, cmd_buffer);
        lcd_trans->flags.dc_gpio_level = spi_panel_io->flags.dc_cmd_level;
        lcd_trans->base.length = spi_panel_io->lcd_cmd_bits;
        lcd_trans->base.tx_buffer = cmd_buffer;
        ret = spi_device_polling_transmit(spi_panel_io->spi_dev, &lcd_trans->base);
        if (ret != ESP_OK) goto err;
    }

    if (param && param_size) {
        // 复制参数以避免修改原始数据
        uint8_t *param_buffer = (uint8_t*)malloc(param_size);
        if (param_buffer) {
            memcpy(param_buffer, param, param_size);
            spi_lcd_expander_prepare_param_buffer(spi_panel_io, param_buffer, param_size);
            lcd_trans->flags.dc_gpio_level = spi_panel_io->flags.dc_param_level;
            lcd_trans->base.length = param_size * 8;
            lcd_trans->base.tx_buffer = param_buffer;
            lcd_trans->base.flags &= ~SPI_TRANS_CS_KEEP_ACTIVE;
            ret = spi_device_polling_transmit(spi_panel_io->spi_dev, &lcd_trans->base);
            free(param_buffer);
        } else {
            ret = ESP_ERR_NO_MEM;
        }
        if (ret != ESP_OK) goto err;
    }

err:
    if (lcd_trans && lcd_trans->flags.cs_level) {
        expander_set_cs(spi_panel_io->expander_handle, spi_panel_io->cs_expander_pin, false);
    }
    spi_device_release_bus(spi_panel_io->spi_dev);
    return ret;
}

static esp_err_t panel_io_spi_expander_tx_color(esp_lcd_panel_io_t *io, int lcd_cmd, const void *color, size_t color_size)
{
    esp_err_t ret = ESP_OK;
    spi_transaction_t *spi_trans = NULL;
    lcd_spi_trans_descriptor_t *lcd_trans = NULL;
    esp_lcd_panel_io_spi_expander_t *spi_panel_io = __containerof(io, esp_lcd_panel_io_spi_expander_t, base);

    ret = spi_device_acquire_bus(spi_panel_io->spi_dev, portMAX_DELAY);
    if (ret != ESP_OK) return ret;

    expander_set_cs(spi_panel_io->expander_handle, spi_panel_io->cs_expander_pin, true);

    bool send_cmd = (lcd_cmd >= 0);
    if (send_cmd) {
        size_t num_trans_inflight = spi_panel_io->num_trans_inflight;
        for (size_t i = 0; i < num_trans_inflight; i++) {
            ret = spi_device_get_trans_result(spi_panel_io->spi_dev, &spi_trans, portMAX_DELAY);
            if (ret != ESP_OK) goto err;
            spi_panel_io->num_trans_inflight--;
        }
        lcd_trans = &spi_panel_io->trans_pool[0];
        memset(lcd_trans, 0, sizeof(lcd_spi_trans_descriptor_t));

        uint8_t cmd_buffer[4];
        memcpy(cmd_buffer, &lcd_cmd, sizeof(lcd_cmd));
        spi_lcd_expander_prepare_cmd_buffer(spi_panel_io, cmd_buffer);
        lcd_trans->base.user = (void*)spi_panel_io;
        lcd_trans->flags.dc_gpio_level = spi_panel_io->flags.dc_cmd_level;
        lcd_trans->base.length = spi_panel_io->lcd_cmd_bits;
        lcd_trans->base.tx_buffer = cmd_buffer;
        if (color && color_size) {
            lcd_trans->base.flags |= SPI_TRANS_CS_KEEP_ACTIVE;
        }
        ret = spi_device_polling_transmit(spi_panel_io->spi_dev, &lcd_trans->base);
        if (ret != ESP_OK) goto err;
    }

    do {
        size_t chunk_size = color_size;

        if (spi_panel_io->num_trans_inflight < spi_panel_io->queue_size) {
            lcd_trans = &spi_panel_io->trans_pool[spi_panel_io->num_trans_inflight];
        } else {
            ret = spi_device_get_trans_result(spi_panel_io->spi_dev, &spi_trans, portMAX_DELAY);
            if (ret != ESP_OK) goto err;
            lcd_trans = __containerof(spi_trans, lcd_spi_trans_descriptor_t, base);
            spi_panel_io->num_trans_inflight--;
        }
        memset(lcd_trans, 0, sizeof(lcd_spi_trans_descriptor_t));

        if (chunk_size > spi_panel_io->spi_trans_max_bytes) {
            chunk_size = spi_panel_io->spi_trans_max_bytes;
            lcd_trans->base.flags |= SPI_TRANS_CS_KEEP_ACTIVE;
        } else {
            lcd_trans->flags.en_trans_done_cb = 1;
            lcd_trans->base.flags &= ~SPI_TRANS_CS_KEEP_ACTIVE;
        }

        lcd_trans->base.user = (void*)spi_panel_io;
        lcd_trans->flags.dc_gpio_level = spi_panel_io->flags.dc_data_level;
        lcd_trans->base.length = chunk_size * 8;
        lcd_trans->base.tx_buffer = color;

        ret = spi_device_queue_trans(spi_panel_io->spi_dev, &lcd_trans->base, portMAX_DELAY);
        if (ret != ESP_OK) goto err;
        spi_panel_io->num_trans_inflight++;

        color = (const uint8_t *)color + chunk_size;
        color_size -= chunk_size;
    } while (color_size > 0);

err:
    if (ret != ESP_OK) {
        expander_set_cs(spi_panel_io->expander_handle, spi_panel_io->cs_expander_pin, false);
    }
    spi_device_release_bus(spi_panel_io->spi_dev);
    return ret;
}

static void lcd_spi_expander_pre_trans_cb(spi_transaction_t *trans)
{
    if (trans && trans->user) {
        esp_lcd_panel_io_spi_expander_t *spi_panel_io = (esp_lcd_panel_io_spi_expander_t*)trans->user;
        lcd_spi_trans_descriptor_t *lcd_trans = __containerof(trans, lcd_spi_trans_descriptor_t, base);
        if (spi_panel_io->dc_gpio_num >= 0) {
            gpio_ll_set_level(&GPIO, (gpio_num_t)spi_panel_io->dc_gpio_num, lcd_trans->flags.dc_gpio_level);
            gpio_ll_output_enable(&GPIO, (gpio_num_t)spi_panel_io->dc_gpio_num);
        }
    }
}
static esp_err_t panel_io_spi_expander_register_event_callbacks(esp_lcd_panel_io_handle_t io, 
                                                                 const esp_lcd_panel_io_callbacks_t *cbs, 
                                                                 void *user_ctx)
{
    esp_lcd_panel_io_spi_expander_t *spi_panel_io = __containerof(io, esp_lcd_panel_io_spi_expander_t, base);
    
    if (spi_panel_io->on_color_trans_done != NULL) {
        ESP_LOGW(TAG, "Callback on_color_trans_done was already set and now it was overwritten!");
    }
    
    spi_panel_io->on_color_trans_done = cbs->on_color_trans_done;
    spi_panel_io->user_ctx = user_ctx;
    
    return ESP_OK;
}
static void lcd_spi_expander_post_trans_color_cb(spi_transaction_t *trans)
{
    if (trans && trans->user) {
        esp_lcd_panel_io_spi_expander_t *spi_panel_io = (esp_lcd_panel_io_spi_expander_t*)trans->user;
        lcd_spi_trans_descriptor_t *lcd_trans = __containerof(trans, lcd_spi_trans_descriptor_t, base);
        
        if (spi_panel_io->dc_gpio_num >= 0) {
            gpio_ll_output_disable(&GPIO, (gpio_num_t)spi_panel_io->dc_gpio_num);
        }

        if (lcd_trans->flags.cs_level) {
            expander_set_cs(spi_panel_io->expander_handle, spi_panel_io->cs_expander_pin, false);
        }

        if (lcd_trans->flags.en_trans_done_cb) {
            if (spi_panel_io->on_color_trans_done) {
                spi_panel_io->on_color_trans_done(&spi_panel_io->base, NULL, spi_panel_io->user_ctx);
            }
        }
    }
}