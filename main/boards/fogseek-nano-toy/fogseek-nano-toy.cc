#include "wifi_board.h"
#include "config.h"
#include "power_manager.h"
#include "led_controller.h"
#include "codecs/es8389_audio_codec.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "mcp_server.h"
#include "lamp_controller.h"
#include "led/single_led.h"
#include "assets/lang_config.h"
#include "adc_battery_monitor.h"
#include "device_state_machine.h"
#include "touch_sensor.h"
#include "motor_controller.h"
#include "radar_sensor.h"
#include <esp_log.h>
#include <driver/rtc_io.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <freertos/task.h>
#include <driver/touch_pad.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>


#define TAG "FogSeekNanoToy"


class FogSeekNanoToy : public WifiBoard
{
private:
    Button boot_button_;
    Button ctrl_button_;
    FogSeekPowerManager power_manager_;
    FogSeekLedController led_controller_;
    FogSeekMotorController motor_controller_;

    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    AudioCodec *audio_codec_ = nullptr;
    esp_timer_handle_t check_idle_timer_ = nullptr;

    TouchSensor touch_sensor_1_;
    TouchSensor touch_sensor_2_;
    RadarSensor radar_sensor_;
   
    bool last_touch2_state_ = false;

    EventGroupHandle_t sensor_event_group_ = nullptr;
    static constexpr uint32_t TOUCH1_PRESSED_EVENT = BIT0;
    static constexpr uint32_t TOUCH1_RELEASED_EVENT = BIT1;
    static constexpr uint32_t TOUCH2_PRESSED_EVENT = BIT2;
    static constexpr uint32_t RADAR_DETECTED_EVENT = BIT3;
    static constexpr uint32_t RADAR_CLEAR_EVENT = BIT4;

    TaskHandle_t audio_task_handle_ = nullptr;
    TaskHandle_t motor_task_handle_ = nullptr;

    static constexpr int LOG_BUFFER_SIZE = 256;
    char log_buffer_[LOG_BUFFER_SIZE];

    


    void InitializeI2c()
    {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)0,
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
    }

    void InitializePowerManager()
    {
        power_pin_config_t power_pin_config = {
            .hold_gpio = PWR_HOLD_GPIO,
            .charging_gpio = PWR_CHARGING_GPIO,
            .charge_done_gpio = PWR_CHARGE_DONE_GPIO,
            .adc_gpio = BATTERY_ADC_GPIO};
        power_manager_.Initialize(&power_pin_config);
    }

    void InitializeLedController()
    {
        led_pin_config_t led_pin_config = {
            .red_gpio = LED_RED_GPIO,
            .green_gpio = LED_GREEN_GPIO};
        led_controller_.InitializeLeds(power_manager_, &led_pin_config);
    }

    void InitializeAudioAmplifier()
    {
        gpio_config_t io_conf;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = (1ULL << AUDIO_CODEC_PA_PIN);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        gpio_config(&io_conf);
        SetAudioAmplifierState(false);
    }

    void SetAudioAmplifierState(bool enable)
    {
        gpio_set_level(AUDIO_CODEC_PA_PIN, enable ? 1 : 0);
    }

    void InitializeButtonCallbacks()
    {
        ctrl_button_.OnClick([this]()
                             {
                                 auto &app = Application::GetInstance();
                                 app.ToggleChatState();
                            
                             });
        ctrl_button_.OnDoubleClick([this]()
                                   {
            auto &app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting)
            {
                EnterWifiConfigMode();
                return;
            } });
        ctrl_button_.OnLongPress([this]()
                                 {
            if (!power_manager_.IsPowerOn()) {
                PowerOn();
            } else {
                PowerOff();
            } });
    }

   


     static void SensorMonitorTask(void *pvParameters)
    {
        auto instance = static_cast<FogSeekNanoToy *>(pvParameters);
        
        ESP_LOGI(TAG, "Sensor monitoring task started (optimized)");
        
        uint32_t touch2_debounce_count = 0;
        const uint32_t DEBOUNCE_THRESHOLD = 2;
        uint32_t last_cap_value = 0;
        
        while (true) {
            uint32_t cap_value = instance->touch_sensor_2_.ReadCapTouchValue();
            uint32_t baseline = instance->touch_sensor_2_.GetBaseline();
            
            cap_value = (last_cap_value * 3 + cap_value) / 4;
            last_cap_value = cap_value;
            
            bool touch2_detected = instance->touch_sensor_2_.IsCapTouchDetected();
            
            if (touch2_detected) {
                if (++touch2_debounce_count >= DEBOUNCE_THRESHOLD) {
                    if (!instance->last_touch2_state_) {
                        instance->last_touch2_state_ = true;
                        xEventGroupSetBits(instance->sensor_event_group_, TOUCH2_PRESSED_EVENT);
                        
                        int32_t delta = (int32_t)cap_value - (int32_t)baseline;
                        snprintf(instance->log_buffer_, instance->LOG_BUFFER_SIZE,
                                 "Touch 2: TOUCHED (val=%lu, delta=%ld)", cap_value, delta);
                        ESP_LOGI(TAG, "%s", instance->log_buffer_);
                    }
                    touch2_debounce_count = DEBOUNCE_THRESHOLD;
                }
            } else {
                if (touch2_debounce_count > 0) {
                    touch2_debounce_count--;
                }
                if (touch2_debounce_count == 0 && instance->last_touch2_state_) {
                    instance->last_touch2_state_ = false;
                    ESP_LOGD(TAG, "Touch 2: RELEASED");
                }
            }
            
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }


     static void AudioTask(void* pvParameters)
    {
        auto instance = static_cast<FogSeekNanoToy*>(pvParameters);
        uint32_t events;
        
        while (true) {
            events = xEventGroupWaitBits(instance->sensor_event_group_, 
                                         TOUCH1_PRESSED_EVENT,
                                         pdTRUE,
                                         pdFALSE,
                                         portMAX_DELAY);
            
            if (events & TOUCH1_PRESSED_EVENT) {
                ESP_LOGI(TAG, "AudioTask: Playing sound");
                auto& app = Application::GetInstance();
                app.PlaySound(Lang::Sounds::OGG_KUNKUN);
            }
        }
    }

    static void MotorTask(void* pvParameters)
    {
        auto instance = static_cast<FogSeekNanoToy*>(pvParameters);
        uint32_t events;
        uint8_t target_duty = 0;
        uint8_t last_logged_duty = 255;
        
        while (true) {
            events = xEventGroupWaitBits(instance->sensor_event_group_, 
                                         TOUCH1_PRESSED_EVENT | TOUCH1_RELEASED_EVENT | TOUCH2_PRESSED_EVENT,
                                         pdTRUE,
                                         pdFALSE,
                                         pdMS_TO_TICKS(10));
            
            if (events & TOUCH1_PRESSED_EVENT) {
                target_duty = 50;
                if (target_duty != last_logged_duty) {
                    ESP_LOGI(TAG, "MotorTask: ON (50%%)");
                    last_logged_duty = target_duty;
                }
            }
            else if (events & TOUCH1_RELEASED_EVENT) {
                target_duty = 100;
                if (target_duty != last_logged_duty) {
                    ESP_LOGI(TAG, "MotorTask: OFF");
                    last_logged_duty = target_duty;
                }
            }
            else if (events & TOUCH2_PRESSED_EVENT) {
                if (target_duty > 10) {
                    target_duty -= 10;
                } else {
                    target_duty = 0;
                }
                ESP_LOGI(TAG, "MotorTask: Increase speed (%d%%)", 100 - target_duty);
                last_logged_duty = target_duty;
            }
            
            if (target_duty <= 100) {
                instance->motor_controller_.SetMotorDutyCycle(target_duty);
            }
        }
    }

 

     void StartSensorMonitoring()
    {
        last_touch2_state_ = touch_sensor_2_.IsCapTouchDetected();
        
        sensor_event_group_ = xEventGroupCreate();
        
        xTaskCreate(AudioTask, "audio_task", 4096, this, 8, &audio_task_handle_);
        
        xTaskCreate(MotorTask, "motor_task", 4096, this, 7, &motor_task_handle_);
        
        xTaskCreate(SensorMonitorTask, "sensor_monitor", 4096, this, 5, NULL);
        
        ESP_LOGI(TAG, "All sensor tasks started");
    }

    void HandleAutoWake()
    {
        auto &app = Application::GetInstance();
        if (app.GetDeviceState() == DeviceState::kDeviceStateIdle)
        {
            auto &app = Application::GetInstance();
            if (power_manager_.IsUsbPowered())
            {
                app.PlaySound(Lang::Sounds::OGG_SUCCESS);
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            app.Schedule([]()
                         {
                            auto &app = Application::GetInstance();
                            app.ToggleChatState(); });
        }
        else
        {
            esp_timer_handle_t check_timer;
            esp_timer_create_args_t timer_args = {};
            timer_args.callback = [](void *arg)
            {
                auto instance = static_cast<FogSeekNanoToy *>(arg);
                instance->HandleAutoWake();
            };
            timer_args.arg = this;
            timer_args.name = "check_idle_timer";
            esp_timer_create(&timer_args, &check_timer);
            esp_timer_start_once(check_timer, 500000);
        }
    }

    void PowerOn()
    {
        power_manager_.PowerOn();
        led_controller_.UpdateLedStatus(power_manager_);

        ESP_LOGI(TAG, "Device powered on.");

        motor_controller_.InitializePwmMotor((gpio_num_t)MOTOR_GPIO, 5000);
        
        touch_sensor_1_.InitializeGpioTouch((gpio_num_t)TOUCH_SENSOR_1_GPIO, true, 
                                            nullptr, TOUCH1_PRESSED_EVENT, TOUCH1_RELEASED_EVENT);
        touch_sensor_2_.InitializeCapTouch(TOUCH_SENSOR_2_CHANNEL, TOUCH_SENSOR_2_THRESHOLD_PERCENT);
        
        radar_sensor_.Initialize((gpio_num_t)RADAR_GPIO, true,
                                sensor_event_group_, RADAR_DETECTED_EVENT, RADAR_CLEAR_EVENT);

        StartSensorMonitoring();


        HandleAutoWake();
    }

    void PowerOff()
    {
        power_manager_.PowerOff();
        led_controller_.UpdateLedStatus(power_manager_);

        Application::GetInstance().SetDeviceState(DeviceState::kDeviceStateIdle);

        ESP_LOGI(TAG, "Device powered off.");
    }

public:
    FogSeekNanoToy() : boot_button_(BOOT_BUTTON_GPIO), ctrl_button_(CTRL_BUTTON_GPIO)
    {
        InitializeI2c();
        InitializePowerManager();
        InitializeLedController();
        InitializeAudioAmplifier();
        InitializeButtonCallbacks();

        power_manager_.SetPowerStateCallback([this](FogSeekPowerManager::PowerState state)
                                             { led_controller_.UpdateLedStatus(power_manager_); });
    }

    virtual AudioCodec *GetAudioCodec() override
    {
        static Es8389AudioCodec audio_codec(
            i2c_bus_,
            (i2c_port_t)0,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8389_ADDR,
            true,
            true);
        return &audio_codec;
    }
    virtual Led *GetLed() override
    {
        return led_controller_.GetGreenLed();
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override
    {
        auto power_state = power_manager_.GetPowerState();
        
        level = power_manager_.ReadBatteryLevel();
        
        charging = (power_state == FogSeekPowerManager::PowerState::USB_POWER_CHARGING);
        
        discharging = (power_state == FogSeekPowerManager::PowerState::BATTERY_POWER || 
                      power_state == FogSeekPowerManager::PowerState::LOW_BATTERY);
        
            ESP_LOGD(TAG, "Battery: level=%d%%, charging=%d, discharging=%d, state=%d", 
                 level, charging, discharging, static_cast<int>(power_state));
        
        return true;
    }

    ~FogSeekNanoToy()
    {
        if (check_idle_timer_) {
            esp_timer_delete(check_idle_timer_);
        }
        
        if (sensor_event_group_) {
            vEventGroupDelete(sensor_event_group_);
        }
        
        if (i2c_bus_)
        {
            i2c_del_master_bus(i2c_bus_);
        }
  
    }
};

DECLARE_BOARD(FogSeekNanoToy);