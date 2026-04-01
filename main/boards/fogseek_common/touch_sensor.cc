#include "touch_sensor.h"
#include <esp_log.h>
#include <freertos/task.h>

#define TAG "TouchSensor"

TouchSensor::TouchSensor() {}

TouchSensor::~TouchSensor()
{
    if (is_cap_touch_)
    {
        touch_pad_fsm_stop();
        touch_pad_deinit();
    }
}

void TouchSensor::InitializeGpioTouch(gpio_num_t gpio_pin)
{
    gpio_pin_ = gpio_pin;
    is_cap_touch_ = false;
    
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << gpio_pin_);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);
    
    vTaskDelay(pdMS_TO_TICKS(50));
     gpio_idle_level_ = 0;  // 空闲状态固定为低电平（0）
    
    ESP_LOGI(TAG, "GPIO%d touch sensor initialized, idle level: %d", 
             gpio_pin_, gpio_idle_level_);
}

void TouchSensor::InitializeCapTouch(touch_pad_t touch_channel, float threshold_percent)
{
    touch_channel_ = touch_channel;
    is_cap_touch_ = true;
    
    ESP_LOGI(TAG, "Initializing capacitive touch sensor on channel %d", touch_channel_);
    
    touch_pad_init();
    touch_pad_config(touch_channel_);
    touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER);
    touch_pad_fsm_start();
    
    vTaskDelay(pdMS_TO_TICKS(50));
    
    CalibrateCapTouch();
    
    ESP_LOGI(TAG, "Capacitive touch sensor initialized - Baseline: %" PRIu32 ", Threshold: %" PRIu32, 
             cap_baseline_, cap_threshold_);
}

void TouchSensor::CalibrateCapTouch()
{
    uint32_t sum = 0;
    for (int i = 0; i < 10; i++)
    {
        uint32_t raw_value = 0;
        touch_pad_read_raw_data(touch_channel_, &raw_value);
        sum += raw_value;
        ESP_LOGD(TAG, "Scan %d: RAW=%" PRIu32, i, raw_value);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    cap_baseline_ = sum / 10;
    cap_threshold_ = (uint32_t)(cap_baseline_ * 0.05f);
    
    touch_pad_set_thresh(touch_channel_, cap_threshold_);
}

bool TouchSensor::ReadGpioTouch()
{
    if (gpio_pin_ == GPIO_NUM_NC)
    {
        return false;
    }
    
    int current_level = gpio_get_level(gpio_pin_);
    bool is_touched = (current_level == 1);
    
    return is_touched;
}

int TouchSensor::GetGpioLevel() const
{
    if (gpio_pin_ == GPIO_NUM_NC)
    {
        return 0;
    }
    return gpio_get_level(gpio_pin_);
}
uint32_t TouchSensor::ReadCapTouchValue()
{
    if (!is_cap_touch_)
    {
        return 0;
    }
    
    uint32_t touch_value = 0;
    touch_pad_read_raw_data(touch_channel_, &touch_value);
    return touch_value;
}

bool TouchSensor::IsCapTouchDetected()
{
    if (!is_cap_touch_)
    {
        return false;
    }
    
    uint32_t touch_value = ReadCapTouchValue();
    int32_t delta = (int32_t)touch_value - (int32_t)cap_baseline_;
    return delta >= (int32_t)cap_threshold_;
}

void TouchSensor::SetTouchCallback(TouchCallback callback)
{
    callback_ = callback;
}