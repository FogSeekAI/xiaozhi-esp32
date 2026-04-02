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
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>


#define TAG "FogSeekNanoToy"

extern "C" void IRAM_ATTR gpio44_isr_handler_c(void);


// 全局去抖计数器（用于中断处理）
static volatile uint32_t g_gpio44_debounce_count = 0;

// 全局事件组指针（用于中断处理）
static volatile EventGroupHandle_t* g_sensor_event_group_ptr = nullptr;

  // GPIO44 中断处理函数（静态）
    void IRAM_ATTR gpio44_isr_handler_c(void* arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    
    uint32_t gpio_status = gpio_get_level(TOUCH_SENSOR_1_GPIO);
    
    if (gpio_status == 1) {
        if (g_sensor_event_group_ptr != NULL) {
                xEventGroupSetBitsFromISR(*g_sensor_event_group_ptr, 
                                          BIT0, 
                                          &xHigherPriorityTaskWoken);
            }
    } else {
        g_gpio44_debounce_count = 0;
            if (g_sensor_event_group_ptr != NULL) {
            xEventGroupSetBitsFromISR(*g_sensor_event_group_ptr, 
                                      BIT1, 
                                      &xHigherPriorityTaskWoken);
        }
    }
    
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

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

    // 事件组用于任务间同步
    EventGroupHandle_t sensor_event_group_ = nullptr;
    static constexpr uint32_t TOUCH1_PRESSED_EVENT = BIT0;
    static constexpr uint32_t TOUCH1_RELEASED_EVENT = BIT1;
    static constexpr uint32_t TOUCH2_PRESSED_EVENT = BIT2;

    // 任务句柄
    TaskHandle_t audio_task_handle_ = nullptr;
    TaskHandle_t motor_task_handle_ = nullptr;

    // 异步日志缓冲区
    static constexpr int LOG_BUFFER_SIZE = 256;
    char log_buffer_[LOG_BUFFER_SIZE];

    


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
        
        ESP_LOGI(TAG, "Sensor monitoring task started (optimized)");
        
        // 电容触摸去抖动
        uint32_t touch2_debounce_count = 0;
        const uint32_t DEBOUNCE_THRESHOLD = 2;
        uint32_t last_cap_value = 0;
        
        while (true) {
            // 读取雷达传感器（低优先级）
            bool radar_state = instance->ReadRadarSensor();
            if (radar_state != instance->last_radar_state_) {
                instance->last_radar_state_ = radar_state;
                snprintf(instance->log_buffer_, instance->LOG_BUFFER_SIZE, 
                         "Radar: %s", radar_state ? "DETECTED" : "NONE");
                ESP_LOGD(TAG, "%s", instance->log_buffer_);
            }
            
            // 电容触摸检测（带平滑滤波）
            uint32_t cap_value = instance->touch_sensor_2_.ReadCapTouchValue();
            uint32_t baseline = instance->touch_sensor_2_.GetBaseline();
            
            // 简单的移动平均滤波
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
            
            // 快速响应周期
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }


     // 初始化电机 PWM
    void InitializeMotorPwm()
    {
        // 首先确保 GPIO 处于正确的状态
        gpio_reset_pin((gpio_num_t)MOTOR_GPIO);
        gpio_set_direction((gpio_num_t)MOTOR_GPIO, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)MOTOR_GPIO, 0);
        
        // 配置 LEDC 定时器
        ledc_timer_config_t ledc_timer = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_12_BIT,
            .timer_num = LEDC_TIMER_0,
            .freq_hz = 5000,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        
        ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));
        
        // 配置 LEDC 通道 - 使用完整的初始化列表
        ledc_channel_config_t ledc_channel = {
            .gpio_num = MOTOR_GPIO,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_0,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
            .hpoint = 0,
            .flags = {
                .output_invert = 0,
            },
        };
        
        ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
        
        motor_duty_cycle_ = 0;
        
        ESP_LOGI(TAG, "Motor PWM initialized on GPIO %d", MOTOR_GPIO);
    }

     // 设置电机占空比 (0-100%)
    void SetMotorDutyCycle(uint8_t percentage)
    {
        if (percentage > 100) {
            percentage = 100;
        }
        
        uint32_t new_duty_cycle = (percentage * 4095) / 100;
        
        // 将百分比转换为 12 位值 (0-4095)
        if (motor_duty_cycle_ != new_duty_cycle) {
            motor_duty_cycle_ = new_duty_cycle;
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, motor_duty_cycle_);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
            
            ESP_LOGI(TAG, "Motor duty cycle set to %d%% (%d)", percentage, motor_duty_cycle_);
        }
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

  


    // 初始化 Touch 1 的硬件中断
     void InitializeTouch1Interrupt()
    {
        g_gpio44_debounce_count = 0;
        g_sensor_event_group_ptr = &sensor_event_group_;
        
        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = (1ULL << TOUCH_SENSOR_1_GPIO);
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
        io_conf.intr_type = GPIO_INTR_ANYEDGE;
        gpio_config(&io_conf);
        
        gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
        gpio_isr_handler_add((gpio_num_t)TOUCH_SENSOR_1_GPIO, gpio44_isr_handler_c, NULL);
        
        ESP_LOGI(TAG, "Touch 1 hardware interrupt initialized on GPIO%d", TOUCH_SENSOR_1_GPIO);
    }


    // 音频播放任务（独立优先级）
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

    // 电机控制任务（独立优先级）
    static void MotorTask(void* pvParameters)
    {
        auto instance = static_cast<FogSeekNanoToy*>(pvParameters);
        uint32_t events;
        uint8_t target_duty = 0;
        uint8_t last_logged_duty = 255;  // 初始化为不可能的值
        
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
                instance->SetMotorDutyCycle(target_duty);
            }
        }
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
        last_touch1_state_ = touch_sensor_1_.ReadGpioTouch();
        last_touch2_state_ = touch_sensor_2_.IsCapTouchDetected();
        
        // 创建事件组
        sensor_event_group_ = xEventGroupCreate();
        
        // 启动音频任务（高优先级）
        xTaskCreate(AudioTask, "audio_task", 4096, this, 8, &audio_task_handle_);
        
        // 启动电机任务（中高优先级）
        xTaskCreate(MotorTask, "motor_task", 4096, this, 7, &motor_task_handle_);
        
        // 启动传感器监控任务（中优先级）
        xTaskCreate(SensorMonitorTask, "sensor_monitor", 4096, this, 5, NULL);
        
        ESP_LOGI(TAG, "All sensor tasks started");
    }

    // Touch 1 按下处理
    void HandleTouch1Pressed()
    {
        ESP_LOGI(TAG, ">>> Touch 1 PRESSED!");
        
        auto &app = Application::GetInstance();
        app.PlaySound(Lang::Sounds::OGG_KUNKUN);
        
        SetMotorDutyCycle(50);
        ESP_LOGI(TAG, ">>> Motor ON (50%% duty)");
    }
    
    // Touch 1 释放处理
    void HandleTouch1Released()
    {
        ESP_LOGI(TAG, ">>> Touch 1 RELEASED");
        SetMotorDutyCycle(100);
        ESP_LOGI(TAG, ">>> Motor OFF");
    }
    
    // Touch 2 触摸处理
    void HandleTouch2Touched()
    {
        uint32_t value = touch_sensor_2_.ReadCapTouchValue();
        int32_t delta = (int32_t)value - (int32_t)touch_sensor_2_.GetBaseline();
        ESP_LOGI(TAG, ">>> Touch 2 TOUCHED! Value: %" PRIu32 ", Delta: %" PRId32, value, delta);
        
        IncreaseMotorDutyCycle(10);
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
        InitializeTouch1Interrupt();
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
        
        if (sensor_event_group_) {
            vEventGroupDelete(sensor_event_group_);
        }
        
        if (i2c_bus_)
        {
            i2c_del_master_bus(i2c_bus_);
        }
        
        // 清理中断处理
        gpio_isr_handler_remove((gpio_num_t)TOUCH_SENSOR_1_GPIO);
  
    }
};

DECLARE_BOARD(FogSeekNanoToy);