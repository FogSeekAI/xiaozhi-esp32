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
#include "led/circular_strip.h"
#include "assets/lang_config.h"
#include "adc_battery_monitor.h"
#include "device_state_machine.h"
#include "mcp_tools.h"
#include "boards/fogseek_common/media_storage.h"
#include <esp_log.h>
#include <driver/rtc_io.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>

#define TAG "FogSeekGlowbies"

class FogSeekGlowbies : public WifiBoard
{
private:
    Button boot_button_;
    Button ctrl_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    Button bt_wifi_connect_button_;
    FogSeekPowerManager power_manager_;
    FogSeekLedController led_controller_;
    CircularStrip *rgb_led_strip_ = nullptr;
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    AudioCodec *audio_codec_ = nullptr;
    esp_timer_handle_t check_idle_timer_ = nullptr;
    MediaStorage *media_storage_ = nullptr;

    // 当前音量值
    int current_volume_ = 70; // 默认音量70

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

    // 初始化LED 控制器
    void InitializeLedController()
    {
        led_pin_config_t led_pin_config = {
            .red_gpio = LED_RED_GPIO,
            .green_gpio = LED_GREEN_GPIO,
            .rgb_gpio = LED_RGB_GPIO,
            .rgb_num_leds = LED_RGB_NUM_LEDS};
        led_controller_.InitializeLeds(power_manager_, &led_pin_config);

        // 从 LED 控制器获取 RGB 灯带实例
        rgb_led_strip_ = led_controller_.GetRgbLedStrip();
    }

    // 初始化扩展板电源使能引脚
    void InitializeExtensionPowerEnable()
    {
        gpio_config_t io_conf;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = (1ULL << EXT_POWER_ENABLE_GPIO);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        gpio_config(&io_conf);
        SetExtensionPowerEnableState(false); // 默认关闭扩展板电源使能
    }

    // 设置扩展板电源使能状态
    void SetExtensionPowerEnableState(bool enable)
    {
        gpio_set_level(EXT_POWER_ENABLE_GPIO, enable ? 1 : 0);
    }

    // 初始化媒体存储
    void InitializeMediaStorage()
    {
        media_storage_ = new MediaStorage();

        // 配置SD卡接口引脚（这些引脚需要根据硬件实际情况定义）
        // 在config.h中应该定义这些GPIO引脚
        media_storage_config_t config = {
            .clk = SD_CLK_GPIO,
            .cmd = SD_CMD_GPIO,
            .d0 = SD_D0_GPIO};

        // 初始化媒体存储
        esp_err_t ret = media_storage_->Initialize(&config);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Media storage initialization failed: %s", esp_err_to_name(ret));
        }
        else
        {
            ESP_LOGI(TAG, "Media storage initialized successfully");

            // 执行简单的测试
            TestMediaStorage();
        }
    }

    // 媒体存储简单测试
    void TestMediaStorage()
    {
        if (!media_storage_ || !media_storage_->is_initialized())
        {
            ESP_LOGE(TAG, "Media storage not initialized, skipping test");
            return;
        }

        ESP_LOGI(TAG, "Starting media storage test...");

        // 测试写入文件
        const char *test_file_path = "/test.txt";
        const char *test_data = "Hello, FogSeek Nano Glowbies!";
        esp_err_t ret = media_storage_->write_file(test_file_path, test_data);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Write file failed: %s", esp_err_to_name(ret));
        }
        else
        {
            ESP_LOGI(TAG, "Write file successful");
        }

        // 检查文件是否存在
        if (media_storage_->file_exists(test_file_path))
        {
            ESP_LOGI(TAG, "File %s exists", test_file_path);

            // 读取文件测试
            ret = media_storage_->read_file(test_file_path);
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "Read file failed: %s", esp_err_to_name(ret));
            }
            else
            {
                ESP_LOGI(TAG, "Read file successful");
            }
        }
        else
        {
            ESP_LOGW(TAG, "File %s does not exist", test_file_path);
        }

        // 打印SD卡信息
        media_storage_->print_info();

        ESP_LOGI(TAG, "Media storage test completed");
    }

    // 初始化按键回调
    void InitializeButtonCallbacks()
    {
        // 音量增加按钮回调
        volume_up_button_.OnClick([this]()
                                  {
                                      current_volume_ += 10;
                                      if (current_volume_ > 100)
                                      {
                                          current_volume_ = 100;
                                      }
                                      ESP_LOGI(TAG, "Volume increased to: %d", current_volume_);
                                      
                                      auto codec = GetAudioCodec();
                                      codec->SetOutputVolume(current_volume_); });

        // 音量减少按钮回调
        volume_down_button_.OnClick([this]()
                                    {
                                        current_volume_ -= 10;
                                        if (current_volume_ < 0)
                                        {
                                            current_volume_ = 0;
                                        }
                                        ESP_LOGI(TAG, "Volume decreased to: %d", current_volume_);
                                        
                                        auto codec = GetAudioCodec();
                                        codec->SetOutputVolume(current_volume_); });

        // 蓝牙WiFi连接按钮回调
        bt_wifi_connect_button_.OnClick([this]()
                                        {
                                            // 循环切换RGB灯带颜色
                                            static int color_index = 0;
                                            switch (color_index)
                                            {
                                            case 0:
                                                rgb_led_strip_->SetAllColor({255, 0, 255}); // 紫色
                                                break;
                                            case 1:
                                                rgb_led_strip_->SetAllColor({0, 255, 0}); // 绿色
                                                break;
                                            case 2:
                                                rgb_led_strip_->SetAllColor({255, 255, 0}); // 黄色
                                                break;
                                            case 3:
                                                rgb_led_strip_->SetAllColor({0, 0, 255}); // 蓝色
                                                break;
                                            case 4:
                                                rgb_led_strip_->SetAllColor({255, 165, 0}); // 橙色
                                                break;
                                            case 5:
                                                rgb_led_strip_->SetAllColor({0, 255, 255}); // 青色
                                                break;
                                            default:
                                                rgb_led_strip_->SetAllColor({255, 255, 255}); // 白色
                                                break;
                                            }
                                            color_index = (color_index + 1) % 7; // 循环使用7种颜色
                                            ESP_LOGI(TAG, "Bluetooth/WiFi connect button pressed"); });
        ctrl_button_.OnClick([this]()
                             {
                                 auto &app = Application::GetInstance();
                                 app.ToggleChatState(); // 切换聊天状态（打断）
                             });
        ctrl_button_.OnDoubleClick([this]()
                                   {
                                    rgb_led_strip_->SetAllColor({0, 0, 0}); // 关灯
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
                auto instance = static_cast<FogSeekGlowbies *>(arg);
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
        SetExtensionPowerEnableState(true);
        ESP_LOGI(TAG, "Device powered on.");

        HandleAutoWake(); // 开机自动唤醒
    }

    // 关机流程
    void PowerOff()
    {
        rgb_led_strip_->SetAllColor({0, 0, 0});
        SetExtensionPowerEnableState(false);
        power_manager_.PowerOff();
        led_controller_.UpdateLedStatus(power_manager_);

        Application::GetInstance().SetDeviceState(DeviceState::kDeviceStateIdle); // 关机后将设备状态设置为空闲，便于下次开机自动唤醒

        ESP_LOGI(TAG, "Device powered off.");
    }

public:
    FogSeekGlowbies() : boot_button_(BOOT_BUTTON_GPIO),
                        ctrl_button_(CTRL_BUTTON_GPIO),
                        volume_up_button_(VOLUME_UP_GPIO),
                        volume_down_button_(VOLUME_DOWN_GPIO),
                        bt_wifi_connect_button_(BT_WIFI_CONNECT_GPIO)
    {
        InitializeI2c();
        InitializePowerManager();
        InitializeLedController();
        InitializeButtonCallbacks();
        // 初始化媒体存储
        // InitializeMediaStorage();

        // 设置电源状态变化回调函数
        power_manager_.SetPowerStateCallback([this](FogSeekPowerManager::PowerState state)
                                             { led_controller_.UpdateLedStatus(power_manager_); });
    }

    virtual Led *GetLed() override
    {
        return led_controller_.GetGreenLed();
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

    ~FogSeekGlowbies()
    {
        if (i2c_bus_)
        {
            i2c_del_master_bus(i2c_bus_);
        }

        // 删除RGB灯带对象
        if (rgb_led_strip_)
        {
            delete rgb_led_strip_;
            rgb_led_strip_ = nullptr;
        }

        // 删除媒体存储对象
        if (media_storage_)
        {
            media_storage_->deinit();
            delete media_storage_;
            media_storage_ = nullptr;
        }
    }
};

DECLARE_BOARD(FogSeekGlowbies);