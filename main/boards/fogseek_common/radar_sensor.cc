#include "radar_sensor.h"
#include <esp_log.h>
#include <freertos/task.h>

#define TAG "RadarSensor"

static RadarSensor* g_radar_sensor_instance = nullptr;

static void IRAM_ATTR radar_isr_handler(void* arg)
{
    if (g_radar_sensor_instance == nullptr) {
        return;
    }
    
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    int level = gpio_get_level(g_radar_sensor_instance->GetGpioPin());
    
    if (level == 1) {
        if (g_radar_sensor_instance->event_group_ != nullptr && 
            g_radar_sensor_instance->detect_event_bit_ != 0) {
            xEventGroupSetBitsFromISR(g_radar_sensor_instance->event_group_, 
                                     g_radar_sensor_instance->detect_event_bit_, 
                                     &xHigherPriorityTaskWoken);
        }
        
        if (g_radar_sensor_instance->callback_) {
            g_radar_sensor_instance->callback_(true);
        }
    } else {
        if (g_radar_sensor_instance->event_group_ != nullptr && 
            g_radar_sensor_instance->clear_event_bit_ != 0) {
            xEventGroupSetBitsFromISR(g_radar_sensor_instance->event_group_, 
                                     g_radar_sensor_instance->clear_event_bit_, 
                                     &xHigherPriorityTaskWoken);
        }
        
        if (g_radar_sensor_instance->callback_) {
            g_radar_sensor_instance->callback_(false);
        }
    }
    
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

RadarSensor::RadarSensor() {}

RadarSensor::~RadarSensor()
{
    if (interrupt_enabled_ && gpio_pin_ != GPIO_NUM_NC) {
        gpio_isr_handler_remove(gpio_pin_);
    }
    
    if (g_radar_sensor_instance == this) {
        g_radar_sensor_instance = nullptr;
    }
}

void RadarSensor::Initialize(gpio_num_t gpio_pin, bool enable_interrupt, 
                            EventGroupHandle_t event_group, 
                            EventBits_t detect_bit, 
                            EventBits_t clear_bit)
{
    gpio_pin_ = gpio_pin;
    interrupt_enabled_ = enable_interrupt;
    event_group_ = event_group;
    detect_event_bit_ = detect_bit;
    clear_event_bit_ = clear_bit;
    
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << gpio_pin_);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = enable_interrupt ? GPIO_INTR_ANYEDGE : GPIO_INTR_DISABLE;
    gpio_config(&io_conf);
    
    if (enable_interrupt) {
        g_radar_sensor_instance = this;
        gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
        gpio_isr_handler_add(gpio_pin_, radar_isr_handler, NULL);
        ESP_LOGI(TAG, "Radar sensor initialized on GPIO %d with interrupt", gpio_pin_);
    } else {
        ESP_LOGI(TAG, "Radar sensor initialized on GPIO %d (polling mode)", gpio_pin_);
    }
    
    initialized_ = true;
}

bool RadarSensor::ReadState()
{
    if (!initialized_ || gpio_pin_ == GPIO_NUM_NC) {
        return false;
    }
    
    return gpio_get_level(gpio_pin_) == 1;
}

void RadarSensor::SetCallback(RadarCallback callback)
{
    callback_ = callback;
}