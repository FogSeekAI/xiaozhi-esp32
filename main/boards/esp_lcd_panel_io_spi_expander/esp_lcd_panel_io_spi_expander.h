#pragma once

#include "esp_lcd_panel_io.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "tca6408a_io_expander.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned int lsb_first: 1;
    unsigned int sio_mode: 1;
    unsigned int dc_high_on_cmd: 1;
    unsigned int dc_low_on_data: 1;
    unsigned int dc_low_on_param: 1;
    unsigned int octal_mode: 1;
    unsigned int quad_mode: 1;
} esp_lcd_panel_io_spi_expander_flags_t;

typedef struct {
    esp_lcd_panel_io_spi_expander_flags_t flags;
    int pclk_hz;
    uint8_t spi_mode;
    int cs_ena_posttrans;
    int cs_ena_pretrans;
    int trans_queue_depth;
    int lcd_cmd_bits;
    int lcd_param_bits;
    int dc_gpio_num;
    uint8_t cs_expander_pin;
    uint8_t bl_pin;
    bool bl_use_expander;
    tca6408a_handle_t *expander_handle;
    esp_lcd_panel_io_color_trans_done_cb_t on_color_trans_done;
    void *user_ctx;
} esp_lcd_panel_io_spi_expander_config_t;

esp_err_t esp_lcd_new_panel_io_spi_expander(spi_host_device_t bus, 
                                            const esp_lcd_panel_io_spi_expander_config_t *io_config, 
                                            esp_lcd_panel_io_handle_t *ret_io);

#ifdef __cplusplus
}
#endif