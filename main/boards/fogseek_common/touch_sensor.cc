#include "touch_sensor.h"
#include <esp_log.h>
#include <freertos/task.h>

#define TAG "TouchSensor"

struct InterruptContext {
    EventGroupHandle_t event_group;
    EventBits_t press_bit;
    EventBits_t release_bit;
};

static InterruptContext* g_interrupt_context = nullptr;

static void IRAM_ATTR gpio_touch_isr_handler(void* arg)
{
    InterruptContext* ctx = static_cast<InterruptContext*>(arg);
    if (ctx == nullptr || ctx->event_group == nullptr) {
        return;
    }
    
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    int level = gpio_get_level(g_interrupt_context != nullptr ? 
                               ((TouchSensor*)g_interrupt_context)->GetGpioPin() : GPIO_NUM_NC);
    
    if (level == 1) {
        if (ctx->press_bit != 0) {
            xEventGroupSetBitsFromISR(ctx->event_group, ctx->press_bit, &xHigherPriorityTaskWoken);
        }
    } else {
        if (ctx->release_bit != 0) {
            xEventGroupSetBitsFromISR(ctx->event_group, ctx->release_bit, &xHigherPriorityTaskWoken);
        }
    }
    
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

TouchSensor::TouchSensor() {}

TouchSensor::~TouchSensor()
{
    if (interrupt_enabled_ && gpio_pin_ != GPIO_NUM_NC) {
        gpio_isr_handler_remove(gpio_pin_);
    }
    
    if (is_cap_touch_)
    {
        touch_pad_fsm_stop();
        touch_pad_deinit();
    }
    
    if (g_interrupt_context != nullptr) {
        delete g_interrupt_context;
        g_interrupt_context = nullptr;
    }
}

void TouchSensor::InitializeGpioTouch(gpio_num_t gpio_pin, bool enable_interrupt, EventGroupHandle_t event_group, EventBits_t press_bit, EventBits_t release_bit)
{
    gpio_pin_ = gpio_pin;
    is_cap_touch_ = false;
    interrupt_enabled_ = enable_interrupt;
    event_group_ = event_group;
    press_event_bit_ = press_bit;
    release_event_bit_ = release_bit;
    
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << gpio_pin_);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    io_conf.intr_type = enable_interrupt ? GPIO_INTR_ANYEDGE : GPIO_INTR_DISABLE;
    gpio_config(&io_conf);
    
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_idle_level_ = 0;
    
    if (enable_interrupt) {
        if (g_interrupt_context == nullptr) {
            g_interrupt_context = new InterruptContext();
        }
        g_interrupt_context->event_group = event_group;
        g_interrupt_context->press_bit = press_bit;
        g_interrupt_context->release_bit = release_bit;
        
        gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
        gpio_isr_handler_add(gpio_pin_, gpio_touch_isr_handler, g_interrupt_context);
        
        ESP_LOGI(TAG, "GPIO%d touch sensor initialized with interrupt support", gpio_pin_);
    } else {
        ESP_LOGI(TAG, "GPIO%d touch sensor initialized (polling mode), idle level: %d", 
                 gpio_pin_, gpio_idle_level_);
    }
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

void TouchSensor::SetPressedCallback(TouchEventCallback callback)
{
    pressed_callback_ = callback;
}

void TouchSensor::SetReleasedCallback(TouchEventCallback callback)
{
    released_callback_ = callback;
}