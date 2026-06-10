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
#include "esp32_camera.h"



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
    Camera* camera_ = nullptr;


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

        uint16_t *buffer = (uint16_t *)heap_caps_malloc(DISPLAY_WIDTH * 2, MALLOC_CAP_DMA);
        if (buffer) {
            for (int i = 0; i < DISPLAY_WIDTH; i++) {
                buffer[i] = 0xF800;  // 红色
            }
            
            for (int y = 0; y < DISPLAY_HEIGHT; y++) {
                esp_lcd_panel_draw_bitmap(lcd_panel_, 0, y, DISPLAY_WIDTH, y + 1, buffer);
            }
            heap_caps_free(buffer);
        }

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
        esp_lcd_panel_io_i2c_config_t tp_io_config = {
            .dev_addr = ESP_LCD_TOUCH_IO_I2C_FT5x06_ADDRESS,
            .control_phase_bytes = 1,
            .dc_bit_offset = 0,
            .lcd_cmd_bits = 8,
            .flags =
            {
                .disable_control_phase = 1,
            }
        };
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

        camera_config_t config = {};
        config.ledc_channel = LEDC_CHANNEL_2;
        config.ledc_timer = LEDC_TIMER_2;
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
        config.pin_sccb_sda = -1;
        config.pin_sccb_scl = CAMERA_PIN_SIOC;
        config.sccb_i2c_port = 1;
        config.pin_pwdn = CAMERA_PIN_PWDN;
        config.pin_reset = CAMERA_PIN_RESET;
        config.xclk_freq_hz = XCLK_FREQ_HZ;
        config.pixel_format = PIXFORMAT_RGB565;
        config.frame_size = FRAMESIZE_QVGA;
        config.jpeg_quality = 12;
        config.fb_count = 1;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

        camera_ = new Esp32Camera(config);
        
        // 标记摄像头已初始化
        camera_initialized_ = true;
        ESP_LOGI(TAG, "Camera initialized successfully");
    }

    std::string ConvertRGB565ToJpeg(camera_fb_t* fb) {
        struct JpegBuffer {
            uint8_t* data = nullptr;
            size_t size = 0;
        };
        
        JpegBuffer jpeg_buf;
        
        bool ok = fmt2jpg_cb(fb->buf, fb->len, fb->width, fb->height, 
                            PIXFORMAT_RGB565, 80,
                            [](void* arg, size_t index, const void* data, size_t len) -> size_t {
                                auto* buffer = static_cast<JpegBuffer*>(arg);
                                if (index == 0 && data != nullptr && len > 0) {
                                    buffer->data = (uint8_t*)malloc(len);
                                    if (buffer->data) {
                                        memcpy(buffer->data, data, len);
                                        buffer->size = len;
                                    }
                                } else if (index > 0 && data != nullptr && len > 0 && buffer->data) {
                                    uint8_t* new_data = (uint8_t*)realloc(buffer->data, buffer->size + len);
                                    if (new_data) {
                                        buffer->data = new_data;
                                        memcpy(buffer->data + buffer->size, data, len);
                                        buffer->size += len;
                                    }
                                }
                                return len;
                            },
                            &jpeg_buf);
        
        if (!ok || !jpeg_buf.data || jpeg_buf.size == 0) {
            if (jpeg_buf.data) {
                free(jpeg_buf.data);
            }
            throw std::runtime_error("Failed to convert RGB565 to JPEG");
        }
        
        std::string result((char*)jpeg_buf.data, jpeg_buf.size);
        free(jpeg_buf.data);
        return result;
    }

    void CaptureAndDisplayPhoto(int timeout_ms) {
        if (!camera_initialized_ || !camera_) {
            throw std::runtime_error("Camera not initialized");
        }

        ClearCapturedPhoto();

        bool success = camera_->Capture();
        if (!success) {
            throw std::runtime_error("Failed to capture photo");
        }

        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) {
            throw std::runtime_error("Failed to get camera frame buffer");
        }

        try {
            if (fb->format == PIXFORMAT_RGB565) {
                std::string jpeg_data = ConvertRGB565ToJpeg(fb);
                
                captured_image_size_ = jpeg_data.size();
                captured_image_ = (uint8_t*)malloc(captured_image_size_);
                if (!captured_image_) {
                    throw std::runtime_error("Failed to allocate memory for captured image");
                }
                memcpy(captured_image_, jpeg_data.data(), captured_image_size_);
            } else if (fb->format == PIXFORMAT_JPEG) {
                captured_image_size_ = fb->len;
                captured_image_ = (uint8_t*)malloc(captured_image_size_);
                if (!captured_image_) {
                    throw std::runtime_error("Failed to allocate memory for captured image");
                }
                memcpy(captured_image_, fb->buf, captured_image_size_);
            } else {
                throw std::runtime_error("Unsupported pixel format");
            }

            esp_camera_fb_return(fb);

            ESP_LOGI(TAG, "Photo captured and stored, size: %zu bytes", captured_image_size_);

        } catch (...) {
            esp_camera_fb_return(fb);
            throw;
        }
    }

    void ClearCapturedPhoto() {
        if (captured_image_) {
            free(captured_image_);
            captured_image_ = nullptr;
            captured_image_size_ = 0;
        }
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
                    std::string("请用中文简单描述这张图片中的内容，主要包括物体、颜色、场景和任何可见的文字。"))
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
        return camera_;
    }
};

DECLARE_BOARD(LichuangDevBoard);
