#include "wifi_board.h"
#include "config.h"
#include "power_manager.h"
#include "led_controller.h"
#include "codecs/no_audio_codec.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "mcp_server.h"
#include "mcp_tools.h"
#include "lamp_controller.h"
#include "led/single_led.h"
#include "assets/lang_config.h"
#include "adc_battery_monitor.h"
#include "device_state_machine.h"
#include <esp_log.h>
#include <driver/rtc_io.h>
#include <wifi_manager.h>

#define TAG "FogSeekAudioAiya"

class FogSeekAudioAiya : public WifiBoard
{
private:
    Button boot_button_;
    Button ctrl_button_;
    FogSeekPowerManager power_manager_;
    FogSeekLedController led_controller_;
    AudioCodec *audio_codec_ = nullptr;
    esp_timer_handle_t check_idle_timer_ = nullptr;
    TaskHandle_t vad_monitor_task_handle_ = nullptr; // 添加一个任务句柄
    bool is_manual_recording_ = false;               // 添加一个标志来跟踪是否正在手动录音

    // 添加一个监控任务，持续确保VAD被唤醒词唤醒后保持禁用
    void StartVadMonitorTask()
    {
        xTaskCreate([](void *pvParameter)
                    {
            FogSeekAudioAiya *instance = static_cast<FogSeekAudioAiya*>(pvParameter);
            TickType_t lastWakeTime = xTaskGetTickCount();
            
            while(1) {
                vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(1000)); // 每秒检查一次
                
                auto &app = Application::GetInstance();
                // 检查当前状态，如果应用试图启用语音处理，且不在手动录音状态，则禁用它
                if (app.GetAudioService().IsAudioProcessorRunning() && !instance->is_manual_recording_) {
                    app.GetAudioService().EnableVoiceProcessing(false);
                    ESP_LOGD(TAG, "VAD was re-enabled by app logic, disabling again");
                }
            } }, "vad_monitor_task", 2048, this, 5, &vad_monitor_task_handle_);
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

    void InitializeButtonCallbacks()
    {
        // 长按：按下按键开始讲话，松开按键结束讲话，将内容发送给AI
        ctrl_button_.OnPressDown([this]()
                                 {
        auto &app = Application::GetInstance();

        // 设置手动录音标志
        is_manual_recording_ = true;

        // 如果设备不在监听状态，则开始监听
        if (app.GetDeviceState() != DeviceState::kDeviceStateListening) {
            app.StartListening();
        }
        ESP_LOGI(TAG, "Started recording - button pressed down"); });

        ctrl_button_.OnPressUp([this]()
                               {
        auto &app = Application::GetInstance();
        // 如果设备在监听状态，则停止监听
        if (app.GetDeviceState() == DeviceState::kDeviceStateListening) {
            app.StopListening();
        }
        
        // 清除手动录音标志
        is_manual_recording_ = false;
        ESP_LOGI(TAG, "Stopped recording - button released"); });

        // 单击：进入WiFi配置模式
        ctrl_button_.OnClick([this]()
                             {
                             auto &app = Application::GetInstance();
                             if (app.GetDeviceState() == DeviceState::kDeviceStateStarting)
                             {
                                 EnterWifiConfigMode();
                                 return;
                             } });

        // 双击：关机
        ctrl_button_.OnDoubleClick([this]()
                                   {
        auto &app = Application::GetInstance();
        
        ESP_LOGI(TAG, "Double click detected, powering off device");
        app.Alert("INFO", "关机中...", "neutral", "");
        PowerOff(); });
    }

    void HandleAutoWake()
    {
        auto &app = Application::GetInstance();
        if (app.GetDeviceState() == DeviceState::kDeviceStateIdle)
        {
            app.GetAudioService().EnableVoiceProcessing(false);
        }
        else
        {
            esp_timer_handle_t check_timer;
            esp_timer_create_args_t timer_args = {};
            timer_args.callback = [](void *arg)
            {
                auto instance = static_cast<FogSeekAudioAiya *>(arg);
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

        HandleAutoWake();
    }

    void PowerOff()
    {
        power_manager_.PowerOff();
        led_controller_.UpdateLedStatus(power_manager_);

        Application::GetInstance().SetDeviceState(DeviceState::kDeviceStateIdle);

        ESP_LOGI(TAG, "Device powered off.");
    }

    void InitializeMCP()
    {
        auto &mcp_server = McpServer::GetInstance();
        InitializeSystemMCP(mcp_server, power_manager_);
    }

public:
    FogSeekAudioAiya() : boot_button_(BOOT_BUTTON_GPIO), ctrl_button_(CTRL_BUTTON_GPIO), is_manual_recording_(false)
    {
        InitializePowerManager();
        InitializeLedController();
        InitializeButtonCallbacks();
        InitializeMCP();
        PowerOn();
        StartVadMonitorTask(); // 启动VAD监控任务

        power_manager_.SetPowerStateCallback([this](FogSeekPowerManager::PowerState state)
                                             { led_controller_.UpdateLedStatus(power_manager_); });
    }

    virtual Led *GetLed() override
    {
        return led_controller_.GetGreenLed();
    }

    virtual AudioCodec *GetAudioCodec() override
    {
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                              AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
        return &audio_codec;
    }

    virtual void StartNetwork() override
    {
        auto &wifi_manager = WifiManager::GetInstance();

        WifiManagerConfig config;
        config.ssid_prefix = "XiaoYa";
        config.language = Lang::CODE;
        wifi_manager.Initialize(config);

        wifi_manager.SetEventCallback([this, &wifi_manager](WifiEvent event)
                                      {
            std::string ssid = wifi_manager.GetSsid();
            switch (event) {
                case WifiEvent::Scanning:
                    OnNetworkEvent(NetworkEvent::Scanning);
                    break;
                case WifiEvent::Connecting:
                    OnNetworkEvent(NetworkEvent::Connecting, ssid);
                    break;
                case WifiEvent::Connected:
                    OnNetworkEvent(NetworkEvent::Connected, ssid);
                    break;
                case WifiEvent::Disconnected:
                    OnNetworkEvent(NetworkEvent::Disconnected);
                    break;
                case WifiEvent::ConfigModeEnter:
                    OnNetworkEvent(NetworkEvent::WifiConfigModeEnter);
                    break;
                case WifiEvent::ConfigModeExit:
                    OnNetworkEvent(NetworkEvent::WifiConfigModeExit);
                    break;
            } });

        TryWifiConnect();
    }

    ~FogSeekAudioAiya()
    {
        // 确保任务被删除
        if (vad_monitor_task_handle_ != nullptr)
        {
            vTaskDelete(vad_monitor_task_handle_);
        }
    }
};

DECLARE_BOARD(FogSeekAudioAiya);