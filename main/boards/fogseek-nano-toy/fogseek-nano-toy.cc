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
#include <esp_log.h>
#include <driver/rtc_io.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <freertos/task.h>
#include <driver/touch_pad.h>
#include <driver/ledc.h>


#define TAG "FogSeekNanoToy"


class FogSeekNanoToy : public WifiBoard
{
private:
    Button boot_button_;
    Button ctrl_button_;
    FogSeekPowerManager power_manager_;
    FogSeekLedController led_controller_;

    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    AudioCodec *audio_codec_ = nullptr;
    esp_timer_handle_t check_idle_timer_ = nullptr;

    TouchSensor touch_sensor_1_;  // GPIO44 普通触摸
    TouchSensor touch_sensor_2_;  // GPIO9 电容触摸
   
    // 传感器状态
    bool last_radar_state_ = false;
    bool last_touch1_state_ = false;
    bool last_touch2_state_ = false;

    // PWM 配置
    bool pwm_initialized_ = false;
    bool motor_enabled_ = false;  // 电机开关状态
    uint32_t motor_duty_cycle_ = 0;  // 当前占空比（0-4095）


    mutable bool gpio43_output_enabled_ = false;
    mutable bool gpio43_current_state_ = false;  // 记录 GPIO43 当前状态

    // 初始化I2C外设
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

    // 初始化电源管理器
    void InitializePowerManager()
    {
        power_pin_config_t power_pin_config = {
            .hold_gpio = PWR_HOLD_GPIO,
            .charging_gpio = PWR_CHARGING_GPIO,
            .charge_done_gpio = PWR_CHARGE_DONE_GPIO,
            .adc_gpio = BATTERY_ADC_GPIO};
        power_manager_.Initialize(&power_pin_config);
    }

    // 初始化LED控制器
    void InitializeLedController()
    {
        led_pin_config_t led_pin_config = {
            .red_gpio = LED_RED_GPIO,
            .green_gpio = LED_GREEN_GPIO};
        led_controller_.InitializeLeds(power_manager_, &led_pin_config);
    }

    // 初始化音频功放引脚并默认关闭功放
    void InitializeAudioAmplifier()
    {
        gpio_config_t io_conf;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = (1ULL << AUDIO_CODEC_PA_PIN);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        gpio_config(&io_conf);
        SetAudioAmplifierState(false); // 默认关闭功放
    }

    // 设置音频功放状态
    void SetAudioAmplifierState(bool enable)
    {
        gpio_set_level(AUDIO_CODEC_PA_PIN, enable ? 1 : 0);
    }

    // 初始化按键回调
    void InitializeButtonCallbacks()
    {
        ctrl_button_.OnClick([this]()
                             {
                                 auto &app = Application::GetInstance();
                                 app.ToggleChatState(); // 切换聊天状态（打断）
                            
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
            // 切换电源状态
            if (!power_manager_.IsPowerOn()) {
                PowerOn();
            } else {
                PowerOff();
            } });
    }

    // 初始化雷达传感器
    void InitializeRadarSensor()
    {
        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = (1ULL << RADAR_GPIO);
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&io_conf);
        ESP_LOGI(TAG, "Radar sensor initialized on GPIO %d", RADAR_GPIO);
    }

   


    // 传感器监控任务
     static void SensorMonitorTask(void *pvParameters)
    {
        auto instance = static_cast<FogSeekNanoToy *>(pvParameters);
        
        ESP_LOGI(TAG, "Sensor monitoring task started");
        
        while (true) {
            // 读取雷达传感器
            bool radar_state = instance->ReadRadarSensor();
            if (radar_state != instance->last_radar_state_) {
                instance->last_radar_state_ = radar_state;
                if (radar_state) {
                    ESP_LOGI(TAG, ">>> Radar: Object DETECTED!");
                } else {
                    ESP_LOGI(TAG, ">>> Radar: No object detected");
                }
            }
            
            // 读取 GPIO44 普通触摸传感器 - 直接读取电平判断状态
            int touch1_level = gpio_get_level(TOUCH_SENSOR_1_GPIO);
            bool touch1_detected = (touch1_level == 1);
            
            // 如果状态发生变化，更新并显示
            if (touch1_detected != instance->last_touch1_state_) {
                instance->last_touch1_state_ = touch1_detected;
                if (touch1_detected) {
                    ESP_LOGI(TAG, ">>> Touch 1 (GPIO%d): PRESSED!", TOUCH_SENSOR_1_GPIO);
                    // 播放小狗叫声
                    auto &app = Application::GetInstance();
                    app.PlaySound(Lang::Sounds::OGG_DOG_VOICE03);
                    ESP_LOGI(TAG, ">>> Playing dog voice");
                    
                    // Touch 1 被按下，开启电机（50% 占空比）
                    instance->SetMotorDutyCycle(50);
                    ESP_LOGI(TAG, ">>> Motor turned ON by Touch 1 (50%% duty cycle)");
                } else {
                    ESP_LOGI(TAG, ">>> Touch 1 (GPIO%d): RELEASED", TOUCH_SENSOR_1_GPIO);
                    // Touch 1 释放，关闭电机
                    instance->SetMotorDutyCycle(100);
                    ESP_LOGI(TAG, ">>> Motor turned OFF by Touch 1");
                }
            }
            
            // 读取 GPIO9 电容触摸传感器
            uint32_t touch2_value = instance->touch_sensor_2_.ReadCapTouchValue();
            int32_t touch2_delta = (int32_t)touch2_value - (int32_t)instance->touch_sensor_2_.GetBaseline();
            bool touch2_detected = instance->touch_sensor_2_.IsCapTouchDetected();
            
            if (touch2_detected != instance->last_touch2_state_) {
                instance->last_touch2_state_ = touch2_detected;
                if (touch2_detected) {
                    ESP_LOGI(TAG, ">>> Touch 2 (GPIO9): TOUCHED! Value: %" PRIu32 
                             ", Delta: %" PRId32,
                             touch2_value, touch2_delta);
                    
                    // Touch 2 被触摸，增加占空比 10%
                    instance->IncreaseMotorDutyCycle(10);
                    ESP_LOGI(TAG, ">>> Motor speed increased by Touch 2");
                } else {
                    ESP_LOGI(TAG, ">>> Touch 2 (GPIO9): RELEASED");
                }
            }
            
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }


     // 初始化电机 PWM
    void InitializeMotorPwm()
    {
          // 配置 LEDC 定时器
        ledc_timer_config_t ledc_timer;
        ledc_timer.speed_mode = LEDC_LOW_SPEED_MODE;
        ledc_timer.duty_resolution = LEDC_TIMER_12_BIT;
        ledc_timer.timer_num = LEDC_TIMER_0;
        ledc_timer.freq_hz = 5000;
        ledc_timer.clk_cfg = LEDC_AUTO_CLK;
        ledc_timer.deconfigure = false;
        
        ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));
        
        // 配置 LEDC 通道
        ledc_channel_config_t ledc_channel;
        ledc_channel.gpio_num = MOTOR_GPIO;
        ledc_channel.speed_mode = LEDC_LOW_SPEED_MODE;
        ledc_channel.channel = LEDC_CHANNEL_0;
        ledc_channel.intr_type = LEDC_INTR_DISABLE;
        ledc_channel.timer_sel = LEDC_TIMER_0;
        ledc_channel.duty = 0;
        ledc_channel.hpoint = 0;
        
        ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
        
        motor_duty_cycle_ = 0;  // 初始占空比为 0
        
        ESP_LOGI(TAG, "Motor PWM initialized on GPIO %d, initial duty: 0%%", MOTOR_GPIO);
   
    }

     // 设置电机占空比 (0-100%)
    void SetMotorDutyCycle(uint8_t percentage)
    {
        if (percentage > 100) {
            percentage = 100;
        }
        
        // 将百分比转换为 12 位值 (0-4095)
        motor_duty_cycle_ = (percentage * 4095) / 100;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, motor_duty_cycle_);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        
        ESP_LOGI(TAG, "Motor duty cycle set to %d%% (%d)", percentage, motor_duty_cycle_);
    }
    
    // 增加电机占空比
    void IncreaseMotorDutyCycle(uint8_t increment)
    {
        uint8_t current_percentage = (motor_duty_cycle_ * 100) / 4095;
        uint8_t new_percentage = current_percentage + increment;
        
        if (new_percentage > 100) {
            new_percentage = 100;
        }
        
        SetMotorDutyCycle(new_percentage);
    }

    // 读取雷达传感器状态
    bool ReadRadarSensor()
    {
        return gpio_get_level(RADAR_GPIO) == 1;
    }

 

    // 启动传感器监控
     void StartSensorMonitoring()
    {
        last_radar_state_ = ReadRadarSensor();
        
        // 读取 GPIO44 初始状态
        last_touch1_state_ = touch_sensor_1_.ReadGpioTouch();
        
        // 读取 GPIO9 初始值
        last_touch2_state_ = touch_sensor_2_.IsCapTouchDetected();
        
        ESP_LOGI(TAG, "Initial sensor states - Radar: %d, Touch1 (GPIO%d): %s, Touch2 (GPIO9): %s", 
                 last_radar_state_, 
                 TOUCH_SENSOR_1_GPIO,
                 last_touch1_state_ ? "TOUCHED" : "RELEASED",
                 last_touch2_state_ ? "TOUCHED" : "RELEASED");
        
        xTaskCreate(SensorMonitorTask, "sensor_monitor", 4096, this, 5, NULL);
        ESP_LOGI(TAG, "Sensor monitoring started");
    }


    // 处理自动唤醒逻辑
    void HandleAutoWake()
    {
        auto &app = Application::GetInstance();
        if (app.GetDeviceState() == DeviceState::kDeviceStateIdle)
        {
            auto &app = Application::GetInstance();
            // USB充电状态下开机需要播放音效
            if (power_manager_.IsUsbPowered())
            {
                app.PlaySound(Lang::Sounds::OGG_SUCCESS);
                vTaskDelay(pdMS_TO_TICKS(500)); // 延时500ms播放音效
            }
            app.Schedule([]()
                         {
                            auto &app = Application::GetInstance();
                            app.ToggleChatState(); });
        }
        else
        {
            // 设备尚未进入空闲状态，500ms后再次检查，使用定时器异步检查，不阻塞当前任务
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
            esp_timer_start_once(check_timer, 500000); // 500ms = 500000微秒
        }
    }

    // 开机流程
    void PowerOn()
    {
        power_manager_.PowerOn();                        // 更新电源状态
        led_controller_.UpdateLedStatus(power_manager_); // 更新LED灯状态

        // auto codec = GetAudioCodec();
        // codec->SetOutputVolume(70); // 开机后将音量设置为默认值
        // SetAudioAmplifierState(true);

        ESP_LOGI(TAG, "Device powered on.");

        // 初始化雷达传感器
        InitializeRadarSensor();

        //初始化电机
        InitializeMotorPwm();
        
        // 初始化触摸传感器
        touch_sensor_1_.InitializeGpioTouch(TOUCH_SENSOR_1_GPIO);
        touch_sensor_2_.InitializeCapTouch(TOUCH_SENSOR_2_CHANNEL, TOUCH_SENSOR_2_THRESHOLD_PERCENT);

        // 启动传感器监控任务
        StartSensorMonitoring();


        HandleAutoWake(); // 开机自动唤醒
    }

    // 关机流程
    void PowerOff()
    {
        power_manager_.PowerOff();
        led_controller_.UpdateLedStatus(power_manager_);

        // auto codec = GetAudioCodec();
        // codec->SetOutputVolume(0); // 关机后将音量设置为默0
        // SetAudioAmplifierState(false);

        Application::GetInstance().SetDeviceState(DeviceState::kDeviceStateIdle); // 关机后将设备状态设置为空闲，便于下次开机自动唤醒

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

        // 设置电源状态变化回调函数，充电时，充电状态变化更新指示灯
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

    ~FogSeekNanoToy()
    {
        if (check_idle_timer_) {
            esp_timer_delete(check_idle_timer_);
        }
        
        if (i2c_bus_)
        {
            i2c_del_master_bus(i2c_bus_);
        }
    }
};

DECLARE_BOARD(FogSeekNanoToy);