#ifndef TOUCH_SENSOR_H
#define TOUCH_SENSOR_H

#include <freertos/FreeRTOS.h>
#include <driver/gpio.h>
#include <driver/touch_pad.h>
#include <esp_timer.h>
#include <freertos/event_groups.h>
#include <functional>

class TouchSensor
{
public:
    struct TouchConfig
    {
        gpio_num_t gpio_pin;
        bool is_cap_touch;
        touch_pad_t touch_channel;
    };

    using TouchCallback = std::function<void(bool touched)>;
    using TouchEventCallback = std::function<void()>;

    TouchSensor();
    ~TouchSensor();

    void InitializeGpioTouch(gpio_num_t gpio_pin, bool enable_interrupt = false, EventGroupHandle_t event_group = nullptr, EventBits_t press_bit = 0, EventBits_t release_bit = 0);
    
    void InitializeCapTouch(touch_pad_t touch_channel, float threshold_percent = 0.05f);

    bool ReadGpioTouch();
    int GetGpioLevel() const;
   
    uint32_t ReadCapTouchValue();
    bool IsCapTouchDetected();

    void SetTouchCallback(TouchCallback callback);
    
    void SetPressedCallback(TouchEventCallback callback);
    void SetReleasedCallback(TouchEventCallback callback);

    uint32_t GetBaseline() const { return cap_baseline_; }
    uint32_t GetThreshold() const { return cap_threshold_; }
    
    bool IsInterruptEnabled() const { return interrupt_enabled_; }
    gpio_num_t GetGpioPin() const { return gpio_pin_; }
    TouchEventCallback GetPressedCallback() const { return pressed_callback_; }
    TouchEventCallback GetReleasedCallback() const { return released_callback_; }

private:
    gpio_num_t gpio_pin_ = GPIO_NUM_NC;
    touch_pad_t touch_channel_ = TOUCH_PAD_MAX;
    bool is_cap_touch_ = false;
    
    int gpio_idle_level_ = 0;
    uint32_t cap_baseline_ = 0;
    uint32_t cap_threshold_ = 0;
    
    bool interrupt_enabled_ = false;
    EventGroupHandle_t event_group_ = nullptr;
    EventBits_t press_event_bit_ = 0;
    EventBits_t release_event_bit_ = 0;
    
    TouchCallback callback_ = nullptr;
    TouchEventCallback pressed_callback_ = nullptr;
    TouchEventCallback released_callback_ = nullptr;
    
    void CalibrateCapTouch();
};

#endif // TOUCH_SENSOR_H