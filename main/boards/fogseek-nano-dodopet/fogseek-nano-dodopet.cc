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
#include "assets.h"
#include "led/single_led.h"
#include "assets/lang_config.h"
#include "adc_battery_monitor.h"
#include "device_state_machine.h"
#include <esp_log.h>
#include <driver/rtc_io.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include "uart_transport.h" // 新增：UART串口传输
#include "wifi_manager.h"

#define TAG "FogSeekNanoDodopetDodopet"

// 动物声音资源列表
static const std::string_view CAT_VOICE_SOUNDS[] = {
    Lang::Sounds::OGG_CAT_VOICE01,
    Lang::Sounds::OGG_CAT_VOICE02,
    Lang::Sounds::OGG_CAT_VOICE03,
    Lang::Sounds::OGG_CAT_VOICE04,
    Lang::Sounds::OGG_CAT_VOICE05,
    Lang::Sounds::OGG_CAT_VOICE06,
    Lang::Sounds::OGG_CAT_VOICE07,
};

static const std::string_view DOG_VOICE_SOUNDS[] = {
    Lang::Sounds::OGG_DOG_VOICE01,
    Lang::Sounds::OGG_DOG_VOICE02,
    Lang::Sounds::OGG_DOG_VOICE03,
    Lang::Sounds::OGG_DOG_VOICE04,
    Lang::Sounds::OGG_DOG_VOICE05,
    Lang::Sounds::OGG_DOG_VOICE06,
    Lang::Sounds::OGG_DOG_VOICE07,
};

class FogSeekNanoDodopet : public WifiBoard
{
private:
    Button boot_button_;
    Button ctrl_button_;
    FogSeekPowerManager power_manager_;
    FogSeekLedController led_controller_;
    UartTransport uart_transport_; // 新增：UART串口传输实例

    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    AudioCodec *audio_codec_ = nullptr;
    esp_timer_handle_t check_idle_timer_ = nullptr;

    uint8_t cat_sound_index_ = 0;
    uint8_t dog_sound_index_ = 0;

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
    // void InitializeAudioAmplifier()
    // {
    //     gpio_config_t io_conf;
    //     io_conf.intr_type = GPIO_INTR_DISABLE;
    //     io_conf.mode = GPIO_MODE_OUTPUT;
    //     io_conf.pin_bit_mask = (1ULL << AUDIO_CODEC_PA_PIN);
    //     io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    //     io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    //     gpio_config(&io_conf);
    //     SetAudioAmplifierState(false); // 默认关闭功放
    // }

    // // 设置音频功放状态
    // void SetAudioAmplifierState(bool enable)
    // {
    //     gpio_set_level(AUDIO_CODEC_PA_PIN, enable ? 1 : 0);
    // }

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
                auto instance = static_cast<FogSeekNanoDodopet *>(arg);
                instance->HandleAutoWake();
            };
            timer_args.arg = this;
            timer_args.name = "check_idle_timer";
            esp_timer_create(&timer_args, &check_timer);
            esp_timer_start_once(check_timer, 500000); // 500ms = 500000微秒
        }
    }

    // 初始化 UART串口（用于ESP-15F透传模块）

    void InitializeUart()
    {
        ESP_LOGI(TAG, "Starting UART initialization...");

        bool init_result = uart_transport_.Initialize(UART_NUM_1, UART_TX_PIN, UART_RX_PIN, 115200);
        ESP_LOGI(TAG, "UART init result: %s", init_result ? "SUCCESS" : "FAILED");

        vTaskDelay(pdMS_TO_TICKS(200));

        ESP_LOGI(TAG, "Starting receive task...");
        uart_transport_.StartReceiveTask([this](uint8_t msg_type, const std::string &content)
                                         { HandleUartMessage(msg_type, content); });
    }

    // 处理 UART 接收到的消息
    void HandleUartMessage(uint8_t msg_type, const std::string &content)
    {
        ESP_LOGI(TAG, "Received: Type=0x%02X, Content=\"%s\"", msg_type, content.c_str());

        bool success = false;

        switch (msg_type)
        {
        case MSG_TYPE_EMOTION:
            HandleEmotion(content);
            success = true;
            break;

        case MSG_TYPE_AUDIO_CONTROL:
            success = HandleAudioControl(content);
            break;

        case MSG_TYPE_VOLUME_CONTROL:
            HandleVolumeControl(content);
            success = true;
            break;

        default:
            ESP_LOGW(TAG, "Unknown message type: 0x%02X", msg_type);
            uart_transport_.SendErrorResponse(ERROR_UNKNOWN_TYPE);
            return;
        }

        // 发送 ACK 应答
        if (success)
        {
            uart_transport_.SendAckResponse(msg_type, ACK_RESULT_SUCCESS);
            ESP_LOGI(TAG, "Sent ACK for message type 0x%02X", msg_type);
        }
        else
        {
            uart_transport_.SendAckResponse(msg_type, ACK_RESULT_FAILURE);
            ESP_LOGW(TAG, "Processing failed for message type 0x%02X", msg_type);
        }
    }

    void HandleEmotion(const std::string &emotion)
    {
        ESP_LOGI(TAG, "Emotion: %s", emotion.c_str());
        // 通过协议层发送文本消息给云端 AI
        auto &app = Application::GetInstance();
        app.WakeWordInvoke(emotion);
    }

    // 播放动物声音
    void PlayAnimalSound(const std::string &animal_prefix)
    {
        // 根据动物类型选择并递增索引
        uint8_t sound_index;
        if (animal_prefix == "cat_voice")
        {
            cat_sound_index_ = (cat_sound_index_ % 7) + 1; // 1-7 循环
            sound_index = cat_sound_index_ - 1;            // 转换为 0-6 的索引

            ESP_LOGI(TAG, "Playing cat voice #%d", sound_index + 1);

            auto &app = Application::GetInstance();
            app.PlaySound(CAT_VOICE_SOUNDS[sound_index]);
        }
        else if (animal_prefix == "dog_voice")
        {
            dog_sound_index_ = (dog_sound_index_ % 7) + 1; // 1-7 循环
            sound_index = dog_sound_index_ - 1;            // 转换为 0-6 的索引

            ESP_LOGI(TAG, "Playing dog voice #%d", sound_index + 1);

            auto &app = Application::GetInstance();
            app.PlaySound(DOG_VOICE_SOUNDS[sound_index]);
        }
        else
        {
            ESP_LOGW(TAG, "Unknown animal prefix: %s", animal_prefix.c_str());
        }
    }

    // 处理音频控制命令
    bool HandleAudioControl(const std::string &animal)
    {
        ESP_LOGI(TAG, "Received audio control command: %s", animal.c_str());

        if (animal == "小猫" || animal == "cat")
        {
            ESP_LOGI(TAG, "Playing cat voice");
            PlayAnimalSound("cat_voice");
            return true;
        }
        else if (animal == "小狗" || animal == "dog" || animal == "狗狗")
        {
            ESP_LOGI(TAG, "Playing dog voice");
            PlayAnimalSound("dog_voice");
            return true;
        }
        else
        {
            ESP_LOGW(TAG, "Unsupported animal: %s", animal.c_str());
            return false;
        }
    }

    // 调整音量
    void AdjustVolume(int delta)
    {
        auto codec = GetAudioCodec();
        int current_volume = codec->output_volume();
        int new_volume = current_volume + delta;

        if (new_volume > 100)
            new_volume = 100;
        else if (new_volume < 0)
            new_volume = 0;

        ESP_LOGI(TAG, "Adjusting volume from %d to %d", current_volume, new_volume);
        codec->SetOutputVolume(new_volume);
    }

    // 设置音量
    void SetVolume(int volume)
    {
        auto codec = GetAudioCodec();

        if (volume > 100)
            volume = 100;
        else if (volume < 0)
            volume = 0;

        ESP_LOGI(TAG, "Setting volume to %d", volume);
        codec->SetOutputVolume(volume);
    }

    // 处理音量控制命令
    void HandleVolumeControl(const std::string &cmd)
    {
        if (cmd == "增大" || cmd == "volume_up")
        {
            AdjustVolume(10);
        }
        else if (cmd == "减小" || cmd == "volume_down")
        {
            AdjustVolume(-10);
        }
        else if (cmd == "最大" || cmd == "volume_max")
        {
            SetVolume(100);
        }
        else
        {
            ESP_LOGW(TAG, "Invalid volume command: %s", cmd.c_str());
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
    FogSeekNanoDodopet() : boot_button_(BOOT_BUTTON_GPIO), ctrl_button_(CTRL_BUTTON_GPIO)
    {
        InitializeI2c();
        InitializePowerManager();
        InitializeLedController();
        // InitializeAudioAmplifier();
        InitializeButtonCallbacks();
        InitializeUart(); // 新增：初始化 UART

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

    // 重写StartNetwork方法，实现自定义Wi-Fi热点名称
    virtual void StartNetwork() override
    {
        auto &wifi_manager = WifiManager::GetInstance();

        // Initialize WiFi manager with custom SSID prefix
        WifiManagerConfig config;
        config.ssid_prefix = "Dodopet";
        config.language = Lang::CODE;
        wifi_manager.Initialize(config);

        // Set unified event callback - forward to NetworkEvent with SSID data
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

        // Try to connect or enter config mode
        TryWifiConnect();
    }
    virtual Led *GetLed() override
    {
        return led_controller_.GetGreenLed();
    }

    ~FogSeekNanoDodopet()
    {
        if (i2c_bus_)
        {
            i2c_del_master_bus(i2c_bus_);
        }
    }
};

DECLARE_BOARD(FogSeekNanoDodopet);