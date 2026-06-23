#ifndef RADAR_SENSOR_H
#define RADAR_SENSOR_H

#include <freertos/FreeRTOS.h>
#include <driver/gpio.h>
#include <freertos/event_groups.h>
#include <functional>

class RadarSensor
{
public:
    using RadarCallback = std::function<void(bool detected)>;

    RadarSensor();
    ~RadarSensor();

    void Initialize(gpio_num_t gpio_pin, bool enable_interrupt = false, 
                   EventGroupHandle_t event_group = nullptr, 
                   EventBits_t detect_bit = 0, 
                   EventBits_t clear_bit = 0);

    bool ReadState();
    
    void SetCallback(RadarCallback callback);
    
    gpio_num_t GetGpioPin() const { return gpio_pin_; }
    bool IsInitialized() const { return initialized_; }
    bool IsInterruptEnabled() const { return interrupt_enabled_; }
    
    EventGroupHandle_t event_group_ = nullptr;
    EventBits_t detect_event_bit_ = 0;
    EventBits_t clear_event_bit_ = 0;
    RadarCallback callback_ = nullptr;

private:
    gpio_num_t gpio_pin_ = GPIO_NUM_NC;
    bool initialized_ = false;
    bool interrupt_enabled_ = false;
};

#endif // RADAR_SENSOR_H