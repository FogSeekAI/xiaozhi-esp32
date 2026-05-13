#include "gc2145_camera_driver.h"
#include "boards/fogseek_common/gc2145_cfg.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <cstring>
#include <stdexcept>
#include <thread>
#include "display/lcd_display.h"
#include "display/lvgl_display/lvgl_image.h"
#include "boards/common/board.h"
#include <esp_camera.h>
#include <driver/ledc.h>
#include <driver/gpio.h>
#include "jpg/image_to_jpeg.h"
#include "board.h"
#include "system_info.h"
#include "network_interface.h"


#define TAG "Gc2145Camera"

#define CAMERA_XCLK_FREQ_HZ 10000000
#define CAMERA_PIN_XCLK     GPIO_NUM_11
#define CAMERA_PIN_PCLK     GPIO_NUM_45
#define CAMERA_PIN_VSYNC    GPIO_NUM_6
#define CAMERA_PIN_HREF     GPIO_NUM_7
#define CAMERA_PIN_D2       GPIO_NUM_41
#define CAMERA_PIN_D3       GPIO_NUM_38
#define CAMERA_PIN_D4       GPIO_NUM_4
#define CAMERA_PIN_D5       GPIO_NUM_40
#define CAMERA_PIN_D6       GPIO_NUM_42
#define CAMERA_PIN_D7       GPIO_NUM_16
#define CAMERA_PIN_D8       GPIO_NUM_12
#define CAMERA_PIN_D9       GPIO_NUM_15

static void InitCameraXclk() {
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_1_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = CAMERA_XCLK_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&timer_cfg);

    ledc_channel_config_t ch_cfg = {
        .gpio_num = CAMERA_PIN_XCLK,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 1,
        .hpoint = 0
    };
    ledc_channel_config(&ch_cfg);
    
    ESP_LOGI(TAG, "XCLK started at %d Hz on GPIO %d", CAMERA_XCLK_FREQ_HZ, CAMERA_PIN_XCLK);
}

Gc2145Camera::Gc2145Camera(i2c_master_bus_handle_t i2c_bus) 
    : i2c_bus_(i2c_bus), i2c_dev_(nullptr), initialized_(false), esp_camera_initialized_(false) {
    
    InitCameraXclk();
    
    vTaskDelay(pdMS_TO_TICKS(50));
    
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = GC2145_SCCB_ADDR,
        .scl_speed_hz = 100000,
    };
    
    esp_err_t ret = i2c_master_bus_add_device(i2c_bus_, &dev_cfg, &i2c_dev_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device: %d", ret);
    } else {
        ESP_LOGI(TAG, "GC2145 I2C device added successfully");
    }
}

Gc2145Camera::~Gc2145Camera() {
    if (encoder_thread_.joinable()) {
        encoder_thread_.join();
    }

    if (i2c_dev_) {
        i2c_master_bus_rm_device(i2c_dev_);
        i2c_dev_ = nullptr;
    }
    
    if (frame_buffer_) {
        heap_caps_free(frame_buffer_);
        frame_buffer_ = nullptr;
    }
    
    if (esp_camera_initialized_) {
        esp_camera_deinit();
        esp_camera_initialized_ = false;
    }
}

esp_err_t Gc2145Camera::WriteReg(uint8_t reg, uint8_t value) {
    if (!i2c_dev_) {
        return ESP_ERR_INVALID_STATE;
    }
    
    uint8_t buffer[2] = {reg, value};
    esp_err_t ret = i2c_master_transmit(i2c_dev_, buffer, 2, pdMS_TO_TICKS(200));
    
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "WriteReg failed: reg=0x%02X, err=%d", reg, ret);
    }
    
    return ret;
}

esp_err_t Gc2145Camera::ReadReg(uint8_t reg, uint8_t* value) {
    if (!i2c_dev_) {
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret;
    
    ret = i2c_master_transmit(i2c_dev_, &reg, 1, pdMS_TO_TICKS(200));
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "ReadReg transmit failed: reg=0x%02X, err=%d", reg, ret);
        return ret;
    }
    
    ret = i2c_master_receive(i2c_dev_, value, 1, pdMS_TO_TICKS(200));
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "ReadReg receive failed: reg=0x%02X, err=%d", reg, ret);
    }
    
    return ret;
}

esp_err_t Gc2145Camera::InitSensor() {
    ESP_LOGI(TAG, "Initializing GC2145 sensor via I2C...");
    
    ESP_LOGI(TAG, "Step 1: Waking up sensor (Write 0xFE=0x80)...");
    esp_err_t ret = WriteReg(0xFE, 0x80);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to wake up sensor");
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    
    ESP_LOGI(TAG, "Step 2: Selecting Page 0 (Write 0xFE=0x00)...");
    ret = WriteReg(0xFE, 0x00);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to select page 0");
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    
    ESP_LOGI(TAG, "Step 3: Reading Chip ID from 0xF0...");
    uint8_t chip_id = 0;
    ret = ReadReg(0xF0, &chip_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read chip ID, error: %d", ret);
        return ret;
    }
    
    ESP_LOGI(TAG, "Chip ID read: 0x%02X (Expected: 0x%02X)", chip_id, GC2145_CHIP_ID);
    
    if (chip_id != GC2145_CHIP_ID) {
        ESP_LOGE(TAG, "Chip ID mismatch!");
        return ESP_ERR_INVALID_VERSION;
    }
    
    ESP_LOGI(TAG, "GC2145 chip ID verified successfully");
    
    ESP_LOGI(TAG, "Step 4: Waiting for sensor stabilization (500ms)...");
    vTaskDelay(pdMS_TO_TICKS(500));
    
    ESP_LOGI(TAG, "Step 5: Writing initialization registers...");
    size_t reg_count = sizeof(gc2145_init_reg_tbl) / sizeof(gc2145_init_reg_tbl[0]);
    for (size_t i = 0; i < reg_count; i++) {
        uint8_t addr = gc2145_init_reg_tbl[i][0];
        uint8_t val = gc2145_init_reg_tbl[i][1];
        
        if (addr == 0x00 && val == 0x00 && i > 10) {
            break;
        }
        
        ret = WriteReg(addr, val);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write register 0x%02X at index %d", addr, (int)i);
            return ret;
        }
    }
    
    ESP_LOGI(TAG, "Step 6: Setting pixel format to RGB565 and output order...");
    ret = WriteReg(0x84, 0xa6);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set pixel format");
        return ret;
    }
    
    ESP_LOGI(TAG, "Step 7: Configuring output data order for LCD compatibility...");
    WriteReg(0xfe, 0x00);
    WriteReg(0x17, 0x15);
    
    initialized_ = true;
    ESP_LOGI(TAG, "GC2145 sensor initialized successfully");
    return ESP_OK;
}

esp_err_t Gc2145Camera::ConfigureResolution(uint16_t width, uint16_t height) {
    ESP_LOGI(TAG, "Configuring resolution: %dx%d", width, height);
    
    uint8_t start_x = (1600 - width) / 2;
    uint8_t start_y = (1200 - height) / 2;
    
    WriteReg(0xfe, 0x00);
    WriteReg(0x17, start_x >> 2);
    WriteReg(0x18, start_x & 0x03);
    WriteReg(0x19, start_y >> 2);
    WriteReg(0x1A, start_y & 0x03);
    WriteReg(0x1B, (width + 7) >> 3);
    WriteReg(0x1C, width & 0x07);
    WriteReg(0x1D, (height + 1) >> 1);
    WriteReg(0x1E, height & 0x01);
    
    WriteReg(0x84, 0xa6);
    
    frame_width_ = width;
    frame_height_ = height;
    frame_buffer_size_ = width * height * 2;
    
    if (frame_buffer_) {
        heap_caps_free(frame_buffer_);
    }
    frame_buffer_ = (uint8_t*)heap_caps_malloc(frame_buffer_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frame_buffer_) {
        ESP_LOGE(TAG, "Failed to allocate frame buffer");
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(TAG, "Frame buffer allocated: %zu bytes", frame_buffer_size_);
    return ESP_OK;
}

esp_err_t Gc2145Camera::InitEspCamera() {
    if (esp_camera_initialized_) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing ESP-Camera for DVP/CSI interface...");
    
    camera_config_t config;
    memset(&config, 0, sizeof(config));
    
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = CAMERA_PIN_D2;
    config.pin_d1 = CAMERA_PIN_D3;
    config.pin_d2 = CAMERA_PIN_D4;
    config.pin_d3 = CAMERA_PIN_D5;
    config.pin_d4 = CAMERA_PIN_D6;
    config.pin_d5 = CAMERA_PIN_D7;
    config.pin_d6 = CAMERA_PIN_D8;
    config.pin_d7 = CAMERA_PIN_D9;
    config.pin_xclk = CAMERA_PIN_XCLK;
    config.pin_pclk = CAMERA_PIN_PCLK;
    config.pin_vsync = CAMERA_PIN_VSYNC;
    config.pin_href = CAMERA_PIN_HREF;
    config.pin_sccb_sda = -1;
    config.pin_sccb_scl = -1;
    config.sccb_i2c_port = 0;
    config.pin_pwdn = GPIO_NUM_NC;
    config.pin_reset = GPIO_NUM_NC;
    config.xclk_freq_hz = CAMERA_XCLK_FREQ_HZ;
    config.pixel_format = PIXFORMAT_RGB565;
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 12;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;
    
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed with error 0x%x", err);
        return err;
    }
    
    esp_camera_initialized_ = true;
    ESP_LOGI(TAG, "ESP-Camera initialized successfully");
    return ESP_OK;
}

void Gc2145Camera::SetExplainUrl(const std::string& url, const std::string& token) {
    explain_url_ = url;
    explain_token_ = token;
}

std::string Gc2145Camera::Explain(const std::string& question) {
    if (explain_url_.empty()) {
        throw std::runtime_error("Image explain URL or token is not set");
    }

    if (!frame_buffer_ || frame_buffer_size_ == 0) {
        throw std::runtime_error("No captured frame available for explanation");
    }

    QueueHandle_t jpeg_queue = xQueueCreate(40, sizeof(JpegChunk));
    if (jpeg_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create JPEG queue");
        throw std::runtime_error("Failed to create JPEG queue");
    }

    encoder_thread_ = std::thread([this, jpeg_queue]() {
        uint16_t w = frame_width_;
        uint16_t h = frame_height_;
        
        bool ok = image_to_jpeg_cb(
            frame_buffer_, frame_buffer_size_, w, h, V4L2_PIX_FMT_RGB565, 80,
            [](void* arg, size_t index, const void* data, size_t len) -> size_t {
                auto jpeg_queue = static_cast<QueueHandle_t>(arg);
                JpegChunk chunk = {.data = nullptr, .len = len};
                if (index == 0 && data != nullptr && len > 0) {
                    chunk.data = (uint8_t*)heap_caps_aligned_alloc(16, len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    if (chunk.data == nullptr) {
                        ESP_LOGE(TAG, "Failed to allocate %zu bytes for JPEG chunk", len);
                        chunk.len = 0;
                    } else {
                        memcpy(chunk.data, data, len);
                    }
                } else {
                    chunk.len = 0;
                }
                xQueueSend(jpeg_queue, &chunk, portMAX_DELAY);
                return len;
            },
            jpeg_queue);

        if (!ok) {
            JpegChunk chunk = {.data = nullptr, .len = 0};
            xQueueSend(jpeg_queue, &chunk, portMAX_DELAY);
        }
    });

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(3);
    std::string boundary = "----ESP32_CAMERA_BOUNDARY";

    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
    if (!explain_token_.empty()) {
        http->SetHeader("Authorization", "Bearer " + explain_token_);
    }
    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    http->SetHeader("Transfer-Encoding", "chunked");
    
    if (!http->Open("POST", explain_url_)) {
        ESP_LOGE(TAG, "Failed to connect to explain URL");
        encoder_thread_.join();
        JpegChunk chunk;
        while (xQueueReceive(jpeg_queue, &chunk, portMAX_DELAY) == pdPASS) {
            if (chunk.data != nullptr) {
                heap_caps_free(chunk.data);
            } else {
                break;
            }
        }
        vQueueDelete(jpeg_queue);
        throw std::runtime_error("Failed to connect to explain URL");
    }

    {
        std::string question_field;
        question_field += "--" + boundary + "\r\n";
        question_field += "Content-Disposition: form-data; name=\"question\"\r\n";
        question_field += "\r\n";
        question_field += question + "\r\n";
        http->Write(question_field.c_str(), question_field.size());
    }
    {
        std::string file_header;
        file_header += "--" + boundary + "\r\n";
        file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n";
        file_header += "Content-Type: image/jpeg\r\n";
        file_header += "\r\n";
        http->Write(file_header.c_str(), file_header.size());
    }

    size_t total_sent = 0;
    bool saw_terminator = false;
    while (true) {
        JpegChunk chunk;
        if (xQueueReceive(jpeg_queue, &chunk, portMAX_DELAY) != pdPASS) {
            ESP_LOGE(TAG, "Failed to receive JPEG chunk");
            break;
        }
        if (chunk.data == nullptr) {
            saw_terminator = true;
            break;
        }
        http->Write((const char*)chunk.data, chunk.len);
        total_sent += chunk.len;
        heap_caps_free(chunk.data);
    }
    
    encoder_thread_.join();
    vQueueDelete(jpeg_queue);

    if (!saw_terminator || total_sent == 0) {
        ESP_LOGE(TAG, "JPEG encoder failed or produced empty output");
        throw std::runtime_error("Failed to encode image to JPEG");
    }

    {
        std::string multipart_footer;
        multipart_footer += "\r\n--" + boundary + "--\r\n";
        http->Write(multipart_footer.c_str(), multipart_footer.size());
    }
    http->Write("", 0);

    if (http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "Failed to upload photo, status code: %d", http->GetStatusCode());
        throw std::runtime_error("Failed to upload photo");
    }

    std::string result = http->ReadAll();
    http->Close();

    size_t remain_stack_size = uxTaskGetStackHighWaterMark(nullptr);
    ESP_LOGI(TAG, "Explain image size=%d bytes, compressed size=%d, remain stack size=%d, question=%s\n%s",
             (int)frame_buffer_size_, (int)total_sent, (int)remain_stack_size, question.c_str(), result.c_str());
    return result;
}

bool Gc2145Camera::Capture() {
    if (!initialized_) {
        ESP_LOGE(TAG, "Camera not initialized");
        return false;
    }
    
    if (!esp_camera_initialized_) {
        ESP_LOGI(TAG, "Initializing ESP-Camera for frame capture...");
        esp_err_t ret = InitEspCamera();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize ESP-Camera: %d", ret);
            return false;
        }
    }
    
    ESP_LOGI(TAG, "Capturing frame from GC2145...");
    
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "Failed to get camera frame buffer");
        return false;
    }
    
    ESP_LOGI(TAG, "Frame captured: %dx%d, format=%d, size=%zu", 
             fb->width, fb->height, fb->format, fb->len);
    
    if (frame_buffer_) {
        heap_caps_free(frame_buffer_);
        frame_buffer_ = nullptr;
    }
    
    frame_buffer_size_ = fb->len;
    frame_width_ = fb->width;
    frame_height_ = fb->height;
    
    frame_buffer_ = (uint8_t*)heap_caps_malloc(frame_buffer_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frame_buffer_) {
        ESP_LOGE(TAG, "Failed to allocate frame buffer");
        esp_camera_fb_return(fb);
        return false;
    }
    
    if (fb->format == PIXFORMAT_RGB565) {
        uint16_t* src = (uint16_t*)fb->buf;
        uint16_t* dst = (uint16_t*)frame_buffer_;
        size_t pixel_count = fb->width * fb->height;
        
        for (size_t i = 0; i < pixel_count; i++) {
            dst[i] = __builtin_bswap16(src[i]);
        }
    } else {
        memcpy(frame_buffer_, fb->buf, frame_buffer_size_);
    }
    
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto lcd_display = dynamic_cast<LcdDisplay*>(display);
    
    if (lcd_display && fb->format == PIXFORMAT_RGB565) {
        size_t image_size = fb->len;
        
        int stride = fb->width * 2;
        try {
            uint8_t* image_data = (uint8_t*)heap_caps_malloc(image_size, MALLOC_CAP_SPIRAM);
            if (image_data) {
                if (fb->format == PIXFORMAT_RGB565) {
                    uint16_t* src = (uint16_t*)fb->buf;
                    uint16_t* dst = (uint16_t*)image_data;
                    size_t pixel_count = fb->width * fb->height;
                    
                    for (size_t i = 0; i < pixel_count; i++) {
                        dst[i] = __builtin_bswap16(src[i]);
                    }
                } else {
                    memcpy(image_data, fb->buf, image_size);
                }
                
                auto lvgl_image = std::make_unique<LvglAllocatedImage>(
                    image_data, image_size, 
                    fb->width, fb->height, 
                    stride, LV_COLOR_FORMAT_RGB565
                );
                lcd_display->SetPreviewImage(std::move(lvgl_image));
                ESP_LOGI(TAG, "Preview displayed on LCD");
                
                esp_timer_handle_t clear_timer;
                esp_timer_create_args_t timer_args = {};
                timer_args.callback = [](void* arg) {
                    auto lcd_disp = static_cast<LcdDisplay*>(arg);
                    if (lcd_disp) {
                        lcd_disp->SetPreviewImage(nullptr);
                        ESP_LOGI(TAG, "Preview cleared");
                    }
                };
                timer_args.arg = lcd_display;
                timer_args.name = "clear_preview_timer";
                esp_timer_create(&timer_args, &clear_timer);
                esp_timer_start_once(clear_timer, 3000000);
            } else {
                ESP_LOGE(TAG, "Failed to allocate memory for image data");
            }
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "Failed to display image: %s", e.what());
        }
    }
    
    esp_camera_fb_return(fb);
    
    return true;
}

bool Gc2145Camera::SetHMirror(bool enabled) {
    if (!initialized_) {
        return false;
    }
    
    uint8_t reg_val = 0;
    ReadReg(0x17, &reg_val);
    
    if (enabled) {
        reg_val |= 0x20;
    } else {
        reg_val &= ~0x20;
    }
    
    esp_err_t ret = WriteReg(0x17, reg_val);
    return (ret == ESP_OK);
}

bool Gc2145Camera::SetVFlip(bool enabled) {
    if (!initialized_) {
        return false;
    }
    
    uint8_t reg_val = 0;
    ReadReg(0x17, &reg_val);
    
    if (enabled) {
        reg_val |= 0x10;
    } else {
        reg_val &= ~0x10;
    }
    
    esp_err_t ret = WriteReg(0x17, reg_val);
    return (ret == ESP_OK);
}

std::string Gc2145Camera::Explain(const std::string& question) {
    if (!initialized_) {
        throw std::runtime_error("Camera not initialized");
    }
    
    return "";
}