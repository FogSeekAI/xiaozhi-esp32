#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/rtc_io.h>
#include <esp_log.h>
#include "adc_battery_monitor.h"
#include "application.h"
#include "assets/lang_config.h"
#include "button.h"
#include "codecs/es8389_audio_codec.h"
#include "config.h"
#include "device_state_machine.h"
#include "display_manager.h"
#include "lamp_controller.h"
#include "led/single_led.h"
#include "led_controller.h"
#include "mcp_server.h"
#include "power_manager.h"
#include "system_reset.h"
#include "wifi_board.h"

#define TAG "FogSeekNanoLcd1_8"

class FogSeekNanoLcd1_8 : public WifiBoard {
private:
    Button boot_button_;
    Button ctrl_button_;
    FogSeekPowerManager power_manager_;
    FogSeekDisplayManager display_manager_;
    FogSeekLedController led_controller_;

    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    AudioCodec* audio_codec_ = nullptr;
    esp_timer_handle_t check_idle_timer_ = nullptr;

    // 初始化I2C外设
    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags =
                {
                    .enable_internal_pullup = 1,
                },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    // 初始化电源管理器
    void InitializePowerManager() {
        power_pin_config_t power_pin_config = {.hold_gpio = PWR_HOLD_GPIO,
                                               .charging_gpio = PWR_CHARGING_GPIO,
                                               .charge_done_gpio = PWR_CHARGE_DONE_GPIO,
                                               .adc_gpio = BATTERY_ADC_GPIO};
        power_manager_.Initialize(&power_pin_config);
    }

    // 初始化LED控制器
    void InitializeLedController() {
        led_pin_config_t led_pin_config = {.red_gpio = LED_RED_GPIO, .green_gpio = LED_GREEN_GPIO};
        led_controller_.InitializeLeds(power_manager_, &led_pin_config);
    }

    // 初始化显示管理器
    void InitializeDisplayManager() {
        lcd_pin_config_t lcd_pin_config = {
            .qspi_d0_gpio = LCD_IO0_GPIO,
            .qspi_d1_gpio = LCD_IO1_GPIO,
            .qspi_d2_gpio = LCD_IO2_GPIO,
            .qspi_d3_gpio = LCD_IO3_GPIO,
            .qspi_cs_gpio = LCD_CS_GPIO,
            .qspi_sclk_gpio = LCD_SCL_GPIO,
            .qspi_im0_gpio = LCD_IM0_GPIO,
            .qspi_im2_gpio = LCD_IM2_GPIO,
            .spi_dc_gpio = LCD_DC_GPIO,
            .spi_reset_gpio = LCD_RESET_GPIO,
            .spi_bl_gpio = LCD_BL_GPIO,
            .width = LCD_H_RES,
            .height = LCD_V_RES,
            .offset_x = DISPLAY_OFFSET_X,
            .offset_y = DISPLAY_OFFSET_Y,
            .mirror_x = DISPLAY_MIRROR_X,
            .mirror_y = DISPLAY_MIRROR_Y,
            .swap_xy = DISPLAY_SWAP_XY,
            .rotation = DISPLAY_ROTATION,
        };
        display_manager_.Initialize(BOARD_LCD_TYPE, &lcd_pin_config);
    }

    // 初始化音频功放引脚并默认关闭功放
    void InitializeAudioAmplifier() {
        gpio_config_t io_conf;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = (1ULL << AUDIO_CODEC_PA_PIN);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        gpio_config(&io_conf);
        SetAudioAmplifierState(false);  // 默认关闭功放
    }

    // 设置音频功放状态
    void SetAudioAmplifierState(bool enable) { gpio_set_level(AUDIO_CODEC_PA_PIN, enable ? 1 : 0); }

    // 初始化按键回调
    void InitializeButtonCallbacks() {
        ctrl_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            app.ToggleChatState();  // 切换聊天状态（打断）
        });
        ctrl_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
        });
        ctrl_button_.OnLongPress([this]() {
            // 切换电源状态
            if (!power_manager_.IsPowerOn()) {
                PowerOn();
            } else {
                PowerOff();
            }
        });
    }

    // 处理自动唤醒逻辑
    void HandleAutoWake() {
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == DeviceState::kDeviceStateIdle) {
            auto& app = Application::GetInstance();
            // USB充电状态下开机需要播放音效
            if (power_manager_.IsUsbPowered()) {
                app.PlaySound(Lang::Sounds::OGG_SUCCESS);
                vTaskDelay(pdMS_TO_TICKS(500));  // 延时500ms播放音效
            }
            app.Schedule([]() {
                auto& app = Application::GetInstance();
                app.ToggleChatState();
            });
        } else {
            // 设备尚未进入空闲状态，500ms后再次检查，使用定时器异步检查，不阻塞当前任务
            esp_timer_handle_t check_timer;
            esp_timer_create_args_t timer_args = {};
            timer_args.callback = [](void* arg) {
                auto instance = static_cast<FogSeekNanoLcd1_8*>(arg);
                instance->HandleAutoWake();
            };
            timer_args.arg = this;
            timer_args.name = "check_idle_timer";
            esp_timer_create(&timer_args, &check_timer);
            esp_timer_start_once(check_timer, 500000);  // 500ms = 500000微秒
        }
    }

    // 开机流程
    void PowerOn() {
        power_manager_.PowerOn();                         // 更新电源状态
        led_controller_.UpdateLedStatus(power_manager_);  // 更新LED灯状态
        display_manager_.SetBrightness(100);

        auto codec = GetAudioCodec();
        codec->SetOutputVolume(70);  // 开机后将音量设置为默认值
        SetAudioAmplifierState(true);

        ESP_LOGI(TAG, "Device powered on.");

        HandleAutoWake();  // 开机自动唤醒
    }

    // 关机流程
    void PowerOff() {
        power_manager_.PowerOff();
        led_controller_.UpdateLedStatus(power_manager_);
        display_manager_.SetBrightness(0);

        auto codec = GetAudioCodec();
        codec->SetOutputVolume(0);  // 关机后将音量设置为默0
        SetAudioAmplifierState(false);

        Application::GetInstance().SetDeviceState(
            DeviceState::kDeviceStateIdle);  // 关机后将设备状态设置为空闲，便于下次开机自动唤醒

        ESP_LOGI(TAG, "Device powered off.");
    }

public:
    FogSeekNanoLcd1_8() : boot_button_(BOOT_BUTTON_GPIO), ctrl_button_(CTRL_BUTTON_GPIO) {
        InitializeI2c();
        InitializePowerManager();
        InitializeLedController();
        InitializeDisplayManager();
        InitializeAudioAmplifier();
        InitializeButtonCallbacks();

        // 设置电源状态变化回调函数，充电时，充电状态变化更新指示灯
        power_manager_.SetPowerStateCallback([this](FogSeekPowerManager::PowerState state) {
            led_controller_.UpdateLedStatus(power_manager_);
        });
    }

    virtual Display* GetDisplay() override { return display_manager_.GetDisplay(); }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8389AudioCodec audio_codec(
            i2c_bus_, (i2c_port_t)0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN, GPIO_NUM_NC, AUDIO_CODEC_ES8389_ADDR, true, true);
        return &audio_codec;
    }

    ~FogSeekNanoLcd1_8() {
        if (i2c_bus_) {
            i2c_del_master_bus(i2c_bus_);
        }
    }
};

DECLARE_BOARD(FogSeekNanoLcd1_8);