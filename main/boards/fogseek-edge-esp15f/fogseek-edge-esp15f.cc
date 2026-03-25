#include "wifi_board.h"
#include "config.h"
#include "power_manager.h"
#include "led_controller.h"
#include "codecs/box_audio_codec.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "mcp_server.h"
#include "lamp_controller.h"
#include "led/single_led.h"
#include "assets/lang_config.h"
#include "adc_battery_monitor.h"
#include "device_state_machine.h"
#include <esp_log.h>
#include <driver/rtc_io.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <driver/uart.h>
#include "uart_transport.h"  // 新增：UART 传输类

#define TAG "FogSeekEdgeEsp15F"

class FogSeekEdgeEsp15F : public WifiBoard
{
private:
    Button boot_button_;
    Button ctrl_button_;
    FogSeekPowerManager power_manager_;
    FogSeekLedController led_controller_;
    UartTransport uart_transport_;  // 添加 UART 传输实例


    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    AudioCodec *audio_codec_ = nullptr;
    esp_timer_handle_t check_idle_timer_ = nullptr;

    // 初始化 I2C外设
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
                auto instance = static_cast<FogSeekEdgeEsp15F *>(arg);
                instance->HandleAutoWake();
            };
            timer_args.arg = this;
            timer_args.name = "check_idle_timer";
            esp_timer_create(&timer_args, &check_timer);
            esp_timer_start_once(check_timer, 500000); // 500ms = 500000微秒
        }
    }

    //串口消息转发

    // 初始化 UART 串口通信
    void InitializeUartTransport()
    {
        uart_transport_.Initialize(UART_PORT, UART_TX_PIN, UART_RX_PIN, UART_BAUD_RATE);
        ESP_LOGI(TAG, "UART transport initialized for WiFi module");
    }

    // 开机流程
    void PowerOn()
    {
        power_manager_.PowerOn();                        // 更新电源状态
        led_controller_.UpdateLedStatus(power_manager_); // 更新LED灯状态

        auto codec = GetAudioCodec();
        codec->SetOutputVolume(70); // 开机后将音量设置为默认值
        SetAudioAmplifierState(true);

        ESP_LOGI(TAG, "Device powered on.");

        // 配置 WiFi 模块（先测试通信并配置基本参数）
        ESP_LOGI(TAG, "Configuring WiFi module...");
        if (!uart_transport_.ConfigureWiFiModule())
        {
            ESP_LOGE(TAG, "Failed to configure WiFi module");
            return;
        }
        
        // 步骤 1: 配置 WiFi 模块（AT 测试、关闭回显、设置 WiFi 模式）
        ESP_LOGI(TAG, "Step 1: Initializing WiFi module...");
        if (!uart_transport_.ConfigureWiFiModule())
        {
            ESP_LOGE(TAG, "Failed to initialize WiFi module");
            return;
        }
        ESP_LOGI(TAG, "WiFi module initialized");
        
        // 步骤 2: 连接到 WiFi
        ESP_LOGI(TAG, "Step 2: Connecting to WiFi...");
        if (uart_transport_.ConnectToWiFi(WIFI_SSID, WIFI_PASSWORD))
        {
            ESP_LOGI(TAG, "✓ WiFi connected");
            
            // 步骤 3: 配置 MQTT
            ESP_LOGI(TAG, "Step 3: Configuring MQTT...");
            if (uart_transport_.ConfigureMQTT(MQTT_CLIENT_ID, MQTT_SERVER_ADDR, MQTT_SERVER_PORT))
            {
                ESP_LOGI(TAG, "✓ MQTT configured");
                
                // 步骤 4: 测试 MQTT 连接
                ESP_LOGI(TAG, "Step 4: Testing MQTT connection...");
                if (uart_transport_.TestMQTTConnection())
                {
                    ESP_LOGI(TAG, "✓ MQTT test passed - Ready to communicate!");
                }
                else
                {
                    ESP_LOGE(TAG, "✗ MQTT test failed");
                }
            }
            else
            {
                ESP_LOGE(TAG, "✗ MQTT configuration failed");
            }
        }
        else
        {
            ESP_LOGE(TAG, "✗ WiFi connection failed");
        }


        HandleAutoWake(); // 开机自动唤醒
    }

    // 关机流程
    void PowerOff()
    {
        power_manager_.PowerOff();
        led_controller_.UpdateLedStatus(power_manager_);

        auto codec = GetAudioCodec();
        codec->SetOutputVolume(0); // 关机后将音量设置为默0
        SetAudioAmplifierState(false);

        Application::GetInstance().SetDeviceState(DeviceState::kDeviceStateIdle); // 关机后将设备状态设置为空闲，便于下次开机自动唤醒

        ESP_LOGI(TAG, "Device powered off.");
    }

public:
    FogSeekEdgeEsp15F() : boot_button_(BOOT_BUTTON_GPIO), ctrl_button_(CTRL_BUTTON_GPIO)
    {
        InitializeI2c();
        InitializePowerManager();
        InitializeLedController();
        InitializeAudioAmplifier();
        InitializeButtonCallbacks();
        InitializeUartTransport();  // 初始化 UART

        PowerOn();


        // 设置电源状态变化回调函数，充电时，充电状态变化更新指示灯
        power_manager_.SetPowerStateCallback([this](FogSeekPowerManager::PowerState state)
                                             { led_controller_.UpdateLedStatus(power_manager_); });

        
  
    }

    

    virtual AudioCodec *GetAudioCodec() override
    {
        static BoxAudioCodec audio_codec(
            i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR,
            AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    // 重写消息通知虚函数 - TTS 消息（AI 助手回复）
    virtual void OnChatMessageReceived(const std::string& role, const std::string& content) override
    {
        ESP_LOGI(TAG, "Chat message received: role=%s, content=%s", role.c_str(), content.c_str());
        
        if (role == "assistant") {
            uart_transport_.SendChatMessage(ROLE_ASSISTANT, content);
            ESP_LOGD(TAG, "Forwarded TTS message to WiFi module: %s", content.c_str());
        } else if (role == "user") {
            uart_transport_.SendChatMessage(ROLE_USER, content);
            ESP_LOGD(TAG, "Forwarded STT message to WiFi module: %s", content.c_str());
        }
    }

    // 重写消息通知虚函数 - LLM 情绪消息
    virtual void OnEmotionReceived(const std::string& emotion) override
    {
      ESP_LOGI(TAG, "Emotion received: %s", emotion.c_str());
        
        bool success = uart_transport_.SendEmotion(emotion);
        ESP_LOGI(TAG, "Forwarded emotion to WiFi module: %s, result=%s", 
                 emotion.c_str(), success ? "SUCCESS" : "FAILED");
  }

    ~FogSeekEdgeEsp15F()
    {
        if (i2c_bus_)
        {
            i2c_del_master_bus(i2c_bus_);
        }
    }
};

DECLARE_BOARD(FogSeekEdgeEsp15F);