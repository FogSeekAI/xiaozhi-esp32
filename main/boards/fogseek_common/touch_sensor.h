#ifndef TOUCH_SENSOR_H
#define TOUCH_SENSOR_H

#include <freertos/FreeRTOS.h>
#include <driver/gpio.h>
#include <driver/touch_pad.h>
#include <esp_timer.h>
#include <functional>

class TouchSensor
{
public:
    struct TouchConfig
    {
        gpio_num_t gpio_pin;      // GPIO 引脚编号
        bool is_cap_touch;        // 是否为电容触摸
        touch_pad_t touch_channel; // 电容触摸通道（非电容触摸可设为 TOUCH_PAD_MAX）
    };

    using TouchCallback = std::function<void(bool touched)>;

    TouchSensor();
    ~TouchSensor();

    // 初始化普通 GPIO 触摸传感器
    void InitializeGpioTouch(gpio_num_t gpio_pin);

    // 初始化电容触摸传感器
    void InitializeCapTouch(touch_pad_t touch_channel, float threshold_percent = 0.05f);

    // 读取传感器状态
    bool ReadGpioTouch();
    int GetGpioLevel() const;  // 新增：获取原始 GPIO 电平
   
    uint32_t ReadCapTouchValue();
    bool IsCapTouchDetected();

    // 设置回调函数
    void SetTouchCallback(TouchCallback callback);

    // 获取基准值和阈值
    uint32_t GetBaseline() const { return cap_baseline_; }
    uint32_t GetThreshold() const { return cap_threshold_; }

private:
    gpio_num_t gpio_pin_ = GPIO_NUM_NC;
    touch_pad_t touch_channel_ = TOUCH_PAD_MAX;
    bool is_cap_touch_ = false;
    
    int gpio_idle_level_ = 0;
    uint32_t cap_baseline_ = 0;
    uint32_t cap_threshold_ = 0;
    
    TouchCallback callback_ = nullptr;
    
    void CalibrateCapTouch();
};

#endif // TOUCH_SENSOR_H