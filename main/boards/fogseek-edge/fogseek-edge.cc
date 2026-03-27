#include "wifi_board.h"
#include "config.h"
#include "tca6408a_io_expander.h"
// #include "tca6408a_power_manager.h"
// #include "tca6408a_led_controller.h"
#include "codecs/box_audio_codec.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
// #include "mcp_server.h"
// #include "lamp_controller.h"
#include "led/single_led.h"
#include "assets/lang_config.h"
#include "adc_battery_monitor.h"
#include "device_state_machine.h"
#include <esp_log.h>
#include <driver/rtc_io.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>

#define TAG "FogSeekEdge"

class FogSeekEdge : public WifiBoard
{
private:
    Button boot_button_;
    // Button ctrl_button_;
    tca6408a_handle_t tca6408a_handle_;
    // TCA6408APowerManager power_manager_;
    // TCA6408ALedController led_controller_;

    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    // AudioCodec *audio_codec_ = nullptr;
    // esp_timer_handle_t check_idle_timer_ = nullptr;

    // 初始化 I2C 外设
    void InitializeI2c()
    {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = I2C_SDA_GPIO,
            .scl_io_num = I2C_SCL_GPIO,
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

    // 初始化 TCA6408A IO 扩展器
    void InitializeTca6408a()
    {
        tca6408a_config_t tca6408a_config = {
            .i2c_bus = i2c_bus_,
            .i2c_address = 0x20,
            .int_gpio = I2C_INT_GPIO,
            .reset_gpio = GPIO_NUM_NC};

        esp_err_t ret = tca6408a_init(&tca6408a_handle_, &tca6408a_config);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to initialize TCA6408A");
            return;
        }

        // 配置音频功放引脚为输出模式并打开功放
        tca6408a_set_gpio_direction(&tca6408a_handle_, TCA6408A_AUDIO_CODEC_PA_PIN, TCA6408A_DIR_OUTPUT);
        tca6408a_set_gpio_level(&tca6408a_handle_, TCA6408A_AUDIO_CODEC_PA_PIN, 1);
        ESP_LOGI(TAG, "TCA6408A initialized successfully");
    }

    // // 初始化电源管理器（使用 TCA6408A）
    // void InitializePowerManager()
    // {
    //     TCA6408APowerManager::power_pin_config_t power_pin_config = {
    //         .charging_gpio = CTRL_BUTTON_GPIO,      // P7 - 充电中检测
    //         .charge_done_gpio = BOOT_BUTTON_GPIO,   // P6 - 充电完成检测
    //         .adc_gpio = BATTERY_ADC_GPIO
    //     };

    //     esp_err_t ret = power_manager_.Initialize(&power_pin_config, i2c_bus_, 0x20);
    //     if (ret != ESP_OK)
    //     {
    //         ESP_LOGE(TAG, "Failed to initialize TCA6408A Power Manager");
    //         return;
    //     }
    // }

    // // 初始化 LED 控制器（使用 TCA6408A）
    // void InitializeLedController()
    // {
    //     led_pin_config_t led_pin_config = {
    //         .rgb_gpio = -1,
    //         .rgb_num_leds = 0,
    //         .cold_light_gpio = -1,
    //         .warm_light_gpio = -1
    //     };

    //     esp_err_t ret = led_controller_.Initialize(&led_pin_config, i2c_bus_, 0x20);
    //     if (ret != ESP_OK)
    //     {
    //         ESP_LOGE(TAG, "Failed to initialize TCA6408A LED Controller");
    //         return;
    //     }
    // }

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
    // void InitializeButtonCallbacks()
    // {
    //     ctrl_button_.OnClick([this]()
    //                          {
    //                              auto &app = Application::GetInstance();
    //                              app.PlaySound(Lang::Sounds::OGG_WELCOME);
    //                              vTaskDelay(pdMS_TO_TICKS(1000)); // 延时 500ms 播放音效
    //                              app.ToggleChatState();           // 切换聊天状态（打断）
    //                          });
    //     ctrl_button_.OnDoubleClick([this]()
    //                                {
    //         auto &app = Application::GetInstance();
    //         if (app.GetDeviceState() == kDeviceStateStarting)
    //         {
    //             EnterWifiConfigMode();
    //             return;
    //         } });
    //     ctrl_button_.OnLongPress([this]()
    //                              {
    //         // 切换电源状态
    //         if (!power_manager_.IsPowerOn()) {
    //             PowerOn();
    //         } else {
    //             PowerOff();
    //         } });
    // }

    // 处理自动唤醒逻辑
    // void HandleAutoWake()
    // {
    //     auto &app = Application::GetInstance();
    //     if (app.GetDeviceState() == DeviceState::kDeviceStateIdle)
    //     {
    //         auto &app = Application::GetInstance();
    //         // USB 充电状态下开机需要播放音效
    //         if (power_manager_.IsUsbPowered())
    //         {
    //             app.PlaySound(Lang::Sounds::OGG_SUCCESS);
    //             vTaskDelay(pdMS_TO_TICKS(500)); // 延时 500ms 播放音效
    //         }
    //         app.Schedule([]()
    //                      {
    //                         auto &app = Application::GetInstance();
    //                         app.ToggleChatState(); });
    //     }
    //     else
    //     {
    //         // 设备尚未进入空闲状态，500ms 后再次检查，使用定时器异步检查，不阻塞当前任务
    //         esp_timer_handle_t check_timer;
    //         esp_timer_create_args_t timer_args = {};
    //         timer_args.callback = [](void *arg)
    //         {
    //             auto instance = static_cast<FogSeekEdge *>(arg);
    //             instance->HandleAutoWake();
    //         };
    //         timer_args.arg = this;
    //         timer_args.name = "check_idle_timer";
    //         esp_timer_create(&timer_args, &check_timer);
    //         esp_timer_start_once(check_timer, 500000); // 500ms = 500000 微秒
    //     }
    // }

    // 开机流程
    // void PowerOn()
    // {
    //     power_manager_.PowerOn();                        // 更新电源状态

    //     // 更新 LED 状态
    //     auto device_state = Application::GetInstance().GetDeviceState();
    //     auto power_state = power_manager_.GetPowerState();
    //     auto device_power_state = power_manager_.GetDevicePowerState();
    //     led_controller_.UpdateLedStatus(device_state, power_state, device_power_state);

    //     auto codec = GetAudioCodec();
    //     codec->SetOutputVolume(70); // 开机后将音量设置为默认值
    //     SetAudioAmplifierState(true);

    //     ESP_LOGI(TAG, "Device powered on.");

    //     HandleAutoWake(); // 开机自动唤醒
    // }

    // 关机流程
    // void PowerOff()
    // {
    //     power_manager_.PowerOff();

    //     // 更新 LED 状态
    //     auto device_state = Application::GetInstance().GetDeviceState();
    //     auto power_state = power_manager_.GetPowerState();
    //     auto device_power_state = power_manager_.GetDevicePowerState();
    //     led_controller_.UpdateLedStatus(device_state, power_state, device_power_state);

    //     auto codec = GetAudioCodec();
    //     codec->SetOutputVolume(0); // 关机后将音量设置为默 0
    //     SetAudioAmplifierState(false);

    //     Application::GetInstance().SetDeviceState(DeviceState::kDeviceStateIdle); // 关机后将设备状态设置为空闲，便于下次开机自动唤醒

    //     ESP_LOGI(TAG, "Device powered off.");
    // }

public:
    // FogSeekEdge() : boot_button_(BOOT_BUTTON_GPIO), ctrl_button_(CTRL_BUTTON_GPIO)
    FogSeekEdge() : boot_button_(BOOT_BUTTON_GPIO)
    {
        InitializeI2c();
        InitializeTca6408a();
        // InitializePowerManager();
        // InitializeLedController();
        // InitializeAudioAmplifier();
        // InitializeButtonCallbacks();

        // 设置电源状态变化回调函数，充电时，充电状态变化更新指示灯
        // power_manager_.SetPowerStateCallback([this](TCA6408APowerManager::PowerState state)
        //                                      {
        //                                          auto &app = Application::GetInstance();
        //                                          auto device_state = app.GetDeviceState();
        //                                          auto device_power_state = power_manager_.GetDevicePowerState();
        //                                          led_controller_.UpdateLedStatus(device_state, state, device_power_state);
        //                                      });
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

    ~FogSeekEdge()
    {
        if (i2c_bus_)
        {
            i2c_del_master_bus(i2c_bus_);
        }
    }
};

DECLARE_BOARD(FogSeekEdge);
