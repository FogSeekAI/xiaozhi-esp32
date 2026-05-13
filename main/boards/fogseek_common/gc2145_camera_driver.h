#pragma once

#include "camera.h"
#include <cstdint>
#include <string>
#include <thread>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <driver/i2c_master.h>
#include <esp_err.h>
#include "esp_camera.h"

#define GC2145_CHIP_ID   0x21
#define GC2145_SCCB_ADDR 0x3C

class Gc2145Camera : public Camera {
private:
    i2c_master_bus_handle_t i2c_bus_;
    i2c_master_dev_handle_t i2c_dev_;
    bool initialized_ = false;
    bool esp_camera_initialized_ = false;
    
    uint8_t* frame_buffer_ = nullptr;
    size_t frame_buffer_size_ = 0;
    uint16_t frame_width_ = 0;
    uint16_t frame_height_ = 0;
    
    std::string explain_url_;
    std::string explain_token_;
    std::thread encoder_thread_;
    
public:
    Gc2145Camera(i2c_master_bus_handle_t i2c_bus);
    virtual ~Gc2145Camera();
    
    virtual void SetExplainUrl(const std::string& url, const std::string& token) override;
    virtual bool Capture() override;
    virtual bool SetHMirror(bool enabled) override;
    virtual bool SetVFlip(bool enabled) override;
    virtual std::string Explain(const std::string& question) override;
    
    esp_err_t InitSensor();
    esp_err_t ConfigureResolution(uint16_t width, uint16_t height);
    
    const uint8_t* GetFrameBuffer() const { return frame_buffer_; }
    size_t GetFrameSize() const { return frame_buffer_size_; }
    uint16_t GetWidth() const { return frame_width_; }
    uint16_t GetHeight() const { return frame_height_; }
};