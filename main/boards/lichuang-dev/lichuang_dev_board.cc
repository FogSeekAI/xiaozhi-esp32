#include "wifi_board.h"
#include "codecs/box_audio_codec.h"
#include "display/lcd_display.h"
#include "display/emote_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "i2c_device.h"
#include "mcp_server.h"
#include "system_info.h"
#include "esp_jpeg_enc.h" // 引入新的 JPEG 编码头文件
#include "esp_jpeg_common.h"




#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_lcd_touch_ft5x06.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>
#include <esp_camera.h>
#include <freertos/task.h>
#include <cstring>
#include <sstream>
#include <thread>
#include <stdexcept>
#include <linux/videodev2.h>


#define TAG "LichuangDevBoard"

class Pca9557 : public I2cDevice {
public:
    Pca9557(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        WriteReg(0x01, 0x03);
        WriteReg(0x03, 0xf8);
    }

    void SetOutputState(uint8_t bit, uint8_t level) {
        uint8_t data = ReadReg(0x01);
        data = (data & ~(1 << bit)) | (level << bit);
        WriteReg(0x01, data);
    }
};

class CustomAudioCodec : public BoxAudioCodec {
private:
    Pca9557* pca9557_;

public:
    CustomAudioCodec(i2c_master_bus_handle_t i2c_bus, Pca9557* pca9557) 
        : BoxAudioCodec(i2c_bus, 
                       AUDIO_INPUT_SAMPLE_RATE, 
                       AUDIO_OUTPUT_SAMPLE_RATE,
                       AUDIO_I2S_GPIO_MCLK, 
                       AUDIO_I2S_GPIO_BCLK, 
                       AUDIO_I2S_GPIO_WS, 
                       AUDIO_I2S_GPIO_DOUT, 
                       AUDIO_I2S_GPIO_DIN,
                       GPIO_NUM_NC, 
                       AUDIO_CODEC_ES8311_ADDR, 
                       AUDIO_CODEC_ES7210_ADDR, 
                       AUDIO_INPUT_REFERENCE),
          pca9557_(pca9557) {
    }

    virtual void EnableOutput(bool enable) override {
        BoxAudioCodec::EnableOutput(enable);
        if (enable) {
            pca9557_->SetOutputState(1, 1);
        } else {
            pca9557_->SetOutputState(1, 0);
        }
    }
};

class LichuangDevBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    i2c_master_dev_handle_t pca9557_handle_;
    Button boot_button_;
    Display* display_;
    Pca9557* pca9557_;
    esp_lcd_panel_handle_t lcd_panel_ = nullptr;
    bool camera_initialized_ = false;
    uint8_t* captured_image_ = nullptr;
    size_t captured_image_size_ = 0;



    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));

        // Initialize PCA9557
        pca9557_ = new Pca9557(i2c_bus_, 0x19);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = GPIO_NUM_40;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = GPIO_NUM_41;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * (DISPLAY_HEIGHT / 2) * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            
            if (camera_initialized_) {
                std::thread([this, &app]() {
                    
                }).detach();
            } else {
                app.ToggleChatState();
            }
        });

        

#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif
    }

    void InitializeSt7789Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = GPIO_NUM_NC;
        io_config.dc_gpio_num = GPIO_NUM_39;
        io_config.spi_mode = 2;
        io_config.pclk_hz = 80 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片ST7789
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
        
        esp_lcd_panel_reset(panel);
        pca9557_->SetOutputState(0, 0);

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, true);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);

        lcd_panel_ = panel;

#if CONFIG_USE_EMOTE_MESSAGE_STYLE
        display_ = new emote::EmoteDisplay(panel, panel_io, DISPLAY_WIDTH, DISPLAY_HEIGHT);
#else
        display_ = new SpiLcdDisplay(panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
#endif
    }

    void InitializeTouch()
    {
        esp_lcd_touch_handle_t tp;
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_HEIGHT,
            .y_max = DISPLAY_WIDTH,
            .rst_gpio_num = GPIO_NUM_NC, // Shared with LCD reset
            .int_gpio_num = GPIO_NUM_NC, 
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = 1,
                .mirror_x = 1,
                .mirror_y = 0,
            },
        };
        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
        tp_io_config.scl_speed_hz = 400000;

        esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle);
        esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &tp);
        assert(tp);

        /* Add touch input (for selected screen) */
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(), 
            .handle = tp,
        };

        if(touch_cfg.disp) {
            lvgl_port_add_touch(&touch_cfg);
        } else {
            ESP_LOGE(TAG, "Touch display is not initialized");
        }
    }

    void InitializeCamera() {
        // 打开摄像头电源 (PCA9557 bit 2)
        pca9557_->SetOutputState(2, 0);

        camera_config_t config;
        config.ledc_channel = LEDC_CHANNEL_0;
        config.ledc_timer = LEDC_TIMER_0;
        config.pin_d0 = CAMERA_PIN_D0;
        config.pin_d1 = CAMERA_PIN_D1;
        config.pin_d2 = CAMERA_PIN_D2;
        config.pin_d3 = CAMERA_PIN_D3;
        config.pin_d4 = CAMERA_PIN_D4;
        config.pin_d5 = CAMERA_PIN_D5;
        config.pin_d6 = CAMERA_PIN_D6;
        config.pin_d7 = CAMERA_PIN_D7;
        config.pin_xclk = CAMERA_PIN_XCLK;
        config.pin_pclk = CAMERA_PIN_PCLK;
        config.pin_vsync = CAMERA_PIN_VSYNC;
        config.pin_href = CAMERA_PIN_HREF;
        config.pin_sccb_sda = -1;  // 使用已初始化的 I2C 接口
        config.pin_sccb_scl = CAMERA_PIN_SIOC;
        config.sccb_i2c_port = 1;  // 使用 I2C port 1 (与音频编解码器共用)
        config.pin_pwdn = CAMERA_PIN_PWDN;
        config.pin_reset = CAMERA_PIN_RESET;
        config.xclk_freq_hz = XCLK_FREQ_HZ;
        config.pixel_format = PIXFORMAT_RGB565;  // GC0308 不支持 JPEG，使用 RGB565
        config.frame_size = FRAMESIZE_QVGA;  // 320x240，与 LCD 尺寸匹配
        config.jpeg_quality = 15;
        config.fb_count = 1;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

        esp_err_t err = esp_camera_init(&config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Camera init failed with error 0x%x", err);
            return;
        }

        sensor_t* s = esp_camera_sensor_get();
        if (s) {
            s->set_hmirror(s, 0);
            s->set_vflip(s, 0);
        }

        camera_initialized_ = true;
        ESP_LOGI(TAG, "Camera initialized successfully (RGB565 format)");
    }

    

    bool CaptureAndDisplayPhoto(int display_duration_ms = 3000) {
        if (!camera_initialized_) {
            ESP_LOGE(TAG, "Camera not initialized");
            return false;
        }

        if (!lcd_panel_) {
            ESP_LOGE(TAG, "LCD panel not initialized");
            return false;
        }

        ESP_LOGI(TAG, "Capturing photo...");
        
        camera_fb_t* old_fb = esp_camera_fb_get();
        if (old_fb) {
            esp_camera_fb_return(old_fb);
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        
        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed");
            return false;
        }

        ESP_LOGI(TAG, "Photo captured: %lux%lu, format: %d, size: %lu bytes",
            fb->width, fb->height, fb->format, fb->len);

        lvgl_port_lock(0);
        lv_timer_enable(false);
        lvgl_port_unlock();
        
        vTaskDelay(pdMS_TO_TICKS(50));

        const int lines_per_chunk = 40;
        
        if (fb->format == PIXFORMAT_RGB565) {
            uint16_t* line_buffer = (uint16_t*)fb->buf;
            
            for (int y = 0; y < fb->height; y += lines_per_chunk) {
                int chunk_height = (y + lines_per_chunk > fb->height) ? 
                                  (fb->height - y) : lines_per_chunk;
                
                uint16_t* chunk_start = line_buffer + (y * fb->width);
                
                esp_lcd_panel_draw_bitmap(lcd_panel_, 
                    0, y, 
                    fb->width, y + chunk_height, 
                    chunk_start);
                
                vTaskDelay(pdMS_TO_TICKS(2));
            }
        }

        ESP_LOGI(TAG, "Photo displayed on LCD for %d ms", display_duration_ms);
        
        esp_camera_fb_return(fb);
        
        vTaskDelay(pdMS_TO_TICKS(display_duration_ms));
        
        lvgl_port_lock(0);
        lv_timer_enable(true);
        lv_obj_invalidate(lv_scr_act());
        lvgl_port_unlock();

        return true;
    }

    

    void ClearCapturedPhoto() {
        if (captured_image_) {
            heap_caps_free(captured_image_);
            captured_image_ = nullptr;
            captured_image_size_ = 0;
            ESP_LOGI(TAG, "Photo cleared from memory");
        }
    }


    std::string ConvertRGB565ToJpeg(camera_fb_t* fb) {
        if (fb->format != PIXFORMAT_RGB565) {
            throw std::runtime_error("Unsupported pixel format");
        }

        ESP_LOGI(TAG, "Converting RGB565 to JPEG...");
        
        size_t rgb888_size = fb->width * fb->height * 3;
        uint8_t* rgb888_buf = (uint8_t*)heap_caps_malloc(rgb888_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        
        if (!rgb888_buf) {
            throw std::runtime_error("Failed to allocate RGB888 buffer");
        }

        uint16_t* src = (uint16_t*)fb->buf;
        uint8_t* dst = rgb888_buf;
        
        for (size_t i = 0; i < fb->width * fb->height; i++) {
            uint16_t pixel = __builtin_bswap16(src[i]);
            
            uint8_t r = (pixel >> 11) & 0x1F;
            uint8_t g = (pixel >> 5) & 0x3F;
            uint8_t b = pixel & 0x1F;
            
            r = (r << 3) | (r >> 2);
            g = (g << 2) | (g >> 4);
            b = (b << 3) | (b >> 2);
            
            *dst++ = r;
            *dst++ = g;
            *dst++ = b;
        }

        jpeg_enc_config_t config = DEFAULT_JPEG_ENC_CONFIG();
        config.width = fb->width;
        config.height = fb->height;
        config.src_type = JPEG_PIXEL_FORMAT_RGB888;
        config.quality = 20;
        config.task_enable = false;

        jpeg_enc_handle_t jpeg_enc = NULL;
        jpeg_error_t err = jpeg_enc_open(&config, &jpeg_enc);
        
        if (err != JPEG_ERR_OK || !jpeg_enc) {
            heap_caps_free(rgb888_buf);
            throw std::runtime_error("Failed to open JPEG encoder");
        }

        size_t jpeg_out_size = rgb888_size / 4;
        uint8_t* jpeg_buf = (uint8_t*)heap_caps_malloc(jpeg_out_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        
        if (!jpeg_buf) {
            jpeg_enc_close(jpeg_enc);
            heap_caps_free(rgb888_buf);
            throw std::runtime_error("Failed to allocate JPEG output buffer");
        }

        int out_size = 0;
        err = jpeg_enc_process(jpeg_enc, rgb888_buf, rgb888_size, jpeg_buf, jpeg_out_size, &out_size);
        
        jpeg_enc_close(jpeg_enc);
        heap_caps_free(rgb888_buf);

        if (err != JPEG_ERR_OK || out_size == 0) {
            heap_caps_free(jpeg_buf);
            throw std::runtime_error("JPEG encoding failed");
        }
        
        ESP_LOGI(TAG, "JPEG encoded: %d bytes", out_size);
        
        std::string jpeg_data((char*)jpeg_buf, out_size);
        heap_caps_free(jpeg_buf);
        
        return jpeg_data;
    }

    void InitializeTools() {
        auto &mcp_server = McpServer::GetInstance();
        
        mcp_server.AddTool("self.system.reconfigure_wifi",
            "End this conversation and enter WiFi configuration mode.\n"
            "**CAUTION** You must ask the user to confirm this action.",
            PropertyList(), [this](const PropertyList& properties) -> std::string {
                EnterWifiConfigMode();
                return "{\"status\": \"entering_wifi_config_mode\"}";
            });

        mcp_server.AddTool("camera.take_photo_and_explain",
            "Captures a photo from the device's camera and uses AI to analyze its content. "
            "Use this tool ONLY when the user explicitly asks about the current visual scene, "
            "objects in front of the camera, or requests image recognition. "
            "Do not use this for general conversation.",
            PropertyList({
                Property("question", kPropertyTypeString, 
                    "请用中文简单描述这张图片中的内容，主要包括物体、颜色、场景和任何可见的文字。")
            }), 
            [this](const PropertyList& properties) -> std::string {
                std::string question;
                try {
                    question = properties["question"].value<std::string>();
                } catch (...) {
                    question = "请用中文详细描述这张图片中的内容";
                }

                if (!camera_initialized_) {
                    ESP_LOGE(TAG, "Camera not initialized");
                    return "{\"error\": \"Camera hardware is not ready\"}";
                }

                ESP_LOGI(TAG, "MCP Tool Invoked: Taking photo with question: %s", question.c_str());
                
                try {
                    CaptureAndDisplayPhoto(3000);
                    
                    std::string description = ExplainImageWithQuestion(question);
                    
                    ESP_LOGI(TAG, "MCP Tool Result: %s", description.c_str());
                    return description; 
                    
                } catch (const std::exception& e) {
                    ESP_LOGE(TAG, "MCP Tool Error: %s", e.what());
                    return std::string("{\"error\": \"Analysis failed: ") + e.what() + "\"}";
                }
            });
    }

    std::string ExplainImageWithQuestion(const std::string& question) {
        std::string explain_url = "http://8.138.244.44:8080/explain";

        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) {
            throw std::runtime_error("Failed to capture image from sensor");
        }

        try {
            std::string jpeg_data;
            if (fb->format == PIXFORMAT_RGB565) {
                jpeg_data = ConvertRGB565ToJpeg(fb);
            } else if (fb->format == PIXFORMAT_JPEG) {
                jpeg_data.assign((char*)fb->buf, fb->len);
            } else {
                esp_camera_fb_return(fb);
                throw std::runtime_error("Unsupported pixel format");
            }

            esp_camera_fb_return(fb);

            auto network = Board::GetInstance().GetNetwork();
            auto http = network->CreateHttp(45); 
            
            std::string boundary = "----ESP32_CAMERA_BOUNDARY";
            http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

            if (!http->Open("POST", explain_url)) {
                throw std::runtime_error("Connection to vision server failed");
            }

            std::string question_part = "--" + boundary + "\r\n"
                "Content-Disposition: form-data; name=\"question\"\r\n\r\n"
                + question + "\r\n";
            http->Write(question_part.c_str(), question_part.size());

            std::string file_header = "--" + boundary + "\r\n"
                "Content-Disposition: form-data; name=\"file\"; filename=\"image.jpg\"\r\n"
                "Content-Type: image/jpeg\r\n\r\n";
            http->Write(file_header.c_str(), file_header.size());

            http->Write(jpeg_data.c_str(), jpeg_data.size());

            std::string footer = "\r\n--" + boundary + "--\r\n";
            http->Write(footer.c_str(), footer.size());

            http->Write("", 0);

            int status_code = http->GetStatusCode();
            if (status_code != 200) {
                std::string error_body = http->ReadAll();
                http->Close();
                throw std::runtime_error("Server returned error " + std::to_string(status_code));
            }

            std::string result = http->ReadAll();
            http->Close();

            return result;

        } catch (...) {
            esp_camera_fb_return(fb);
            throw;
        }
    }

public:
    LichuangDevBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeSt7789Display();
        //InitializeTouch();
        InitializeButtons();
        InitializeCamera();
        InitializeTools();

        GetBacklight()->RestoreBrightness();
    }

    virtual ~LichuangDevBoard() {
        ClearCapturedPhoto();
        if (camera_initialized_) {
            esp_camera_deinit();
        }
    }
    virtual AudioCodec* GetAudioCodec() override {
        static CustomAudioCodec audio_codec(
            i2c_bus_, 
            pca9557_);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual Camera* GetCamera() override {
        return nullptr;
    }
};

DECLARE_BOARD(LichuangDevBoard);
