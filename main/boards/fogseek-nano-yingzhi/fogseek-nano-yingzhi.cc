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
#include "ac7065e_transport.h"
#include <esp_log.h>
#include <driver/rtc_io.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>

#define TAG "FogSeekNanoYingZhi"

class FogSeekNanoYingZhi : public WifiBoard
{
private:
    Button boot_button_;
    Button ctrl_button_;
    FogSeekPowerManager power_manager_;
    FogSeekLedController led_controller_;
    AC7065ETransport ac7065e_transport_;

    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    AudioCodec *audio_codec_ = nullptr;
    esp_timer_handle_t check_idle_timer_ = nullptr;

    // AUX 状态标记
    bool aux_inserted_ = false;

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

    // 初始化 AC7065E UART 通信
    void InitializeAC7065EUart()
    {
        ESP_LOGI(TAG, "Initializing AC7065E UART...");

        bool init_result = ac7065e_transport_.Initialize(UART_NUM_1,
                                                         AC7065E_UART_TX_PIN,
                                                         AC7065E_UART_RX_PIN,
                                                         AC7065E_UART_BAUD_RATE);
        ESP_LOGI(TAG, "AC7065E UART init result: %s", init_result ? "SUCCESS" : "FAILED");

        vTaskDelay(pdMS_TO_TICKS(200));

        ESP_LOGI(TAG, "Starting AC7065E receive task...");
        ac7065e_transport_.StartReceiveTask([this](uint8_t cmd, const uint8_t* data, uint8_t len)
                                            { HandleAC7065EMessage(cmd, data, len); });
    }

    // 处理 AC7065E 上报消息
    void HandleAC7065EMessage(uint8_t cmd, const uint8_t* data, uint8_t len)
    {
        ESP_LOGI(TAG, "AC7065E message: cmd=0x%02X, len=%d", cmd, len);

        switch (cmd)
        {
        case CMD_POWER_ON:  // 0x80 - 开关机状态上报
            if (len >= 1)
            {
                bool power_on = (data[0] == 0x01);
                ESP_LOGI(TAG, "AC7065E power state: %s", power_on ? "ON" : "OFF");
            }
            break;

        case CMD_AUX_INSERT:  // 0x81 - AUX线插入
            ESP_LOGI(TAG, "AUX cable INSERTED - disabling AI audio");
            aux_inserted_ = true;
            HandleAuxInserted();
            break;

        case CMD_AUX_REMOVE:  // 0x82 - AUX线拔出
            ESP_LOGI(TAG, "AUX cable REMOVED - AI audio can resume");
            aux_inserted_ = false;
            HandleAuxRemoved();
            break;

        case CMD_CMD_ACK:  // 0x83 - 确认应答
            if (len >= 1)
            {
                ESP_LOGI(TAG, "AC7065E ACK for cmd: 0x%02X", data[0]);
            }
            break;

        case CMD_BATTERY_RESP:  // 0x84 - 电量响应
            if (len >= 1)
            {
                ESP_LOGI(TAG, "AC7065E battery level: %d%%", data[0]);
            }
            break;

        case CMD_VOL_RESP:  // 0x85 - 音量响应
            if (len >= 1)
            {
                ESP_LOGI(TAG, "AC7065E volume level: %d%%", data[0]);
            }
            break;

        case CMD_ERROR:  // 0xFF - 错误
            if (len >= 2)
            {
                ESP_LOGW(TAG, "AC7065E error: cmd=0x%02X, error_type=0x%02X", data[0], data[1]);
            }
            break;

        default:
            ESP_LOGW(TAG, "Unknown AC7065E command: 0x%02X", cmd);
            break;
        }
    }

    // AUX 插入处理: 关闭AI语音音频和功放
    void HandleAuxInserted()
    {
        auto &app = Application::GetInstance();

        // 如果正在对话中，结束对话
        if (app.GetDeviceState() == kDeviceStateListening ||
            app.GetDeviceState() == kDeviceStateSpeaking)
        {
            app.SetDeviceState(kDeviceStateIdle);
        }

        // 关闭功放
        SetAudioAmplifierState(false);

        ESP_LOGI(TAG, "AUX mode: AI audio disabled, amplifier off");
    }

    // AUX 拔出处理: 恢复AI功能
    void HandleAuxRemoved()
    {
        // 恢复功放
        SetAudioAmplifierState(true);

        ESP_LOGI(TAG, "AUX removed: AI audio restored, amplifier on");
    }

    // ============================================================
    // MCP 工具注册 (AI 可通过调用接口下发 AC7065E 命令)
    // ============================================================
    void InitializeTools()
    {
        auto &mcp_server = McpServer::GetInstance();

        // ---- 唤醒/休眠控制 ----

        mcp_server.AddTool("self.ac7065e.wakeup",
            "使 AC7065E 蓝牙音频协处理器进入 AI 语音对话模式",
            PropertyList(),
            [this](const PropertyList &properties) -> ReturnValue
            {
                bool ok = ac7065e_transport_.SendWakeUp();
                ESP_LOGI(TAG, "MCP: WAKEUP -> %s", ok ? "OK" : "FAIL");
                return ok;
            });

        mcp_server.AddTool("self.ac7065e.sleep",
            "关闭 AI 语音对话模式，切换回蓝牙模式",
            PropertyList(),
            [this](const PropertyList &properties) -> ReturnValue
            {
                bool ok = ac7065e_transport_.SendSleep();
                ESP_LOGI(TAG, "MCP: SLEEP -> %s", ok ? "OK" : "FAIL");
                return ok;
            });

        // ---- 音源切换 ----

        mcp_server.AddTool("self.ac7065e.play",
            "恢复蓝牙音乐播放",
            PropertyList(),
            [this](const PropertyList &properties) -> ReturnValue
            {
                bool ok = ac7065e_transport_.SendPlay();
                ESP_LOGI(TAG, "MCP: PLAY -> %s", ok ? "OK" : "FAIL");
                return ok;
            });

        mcp_server.AddTool("self.ac7065e.pause",
            "暂停蓝牙音乐",
            PropertyList(),
            [this](const PropertyList &properties) -> ReturnValue
            {
                bool ok = ac7065e_transport_.SendPause();
                ESP_LOGI(TAG, "MCP: PAUSE -> %s", ok ? "OK" : "FAIL");
                return ok;
            });

        mcp_server.AddTool("self.ac7065e.next_track",
            "切换到下一曲",
            PropertyList(),
            [this](const PropertyList &properties) -> ReturnValue
            {
                bool ok = ac7065e_transport_.SendNextTrack();
                ESP_LOGI(TAG, "MCP: NEXT_TRACK -> %s", ok ? "OK" : "FAIL");
                return ok;
            });

        mcp_server.AddTool("self.ac7065e.prev_track",
            "切换到上一曲",
            PropertyList(),
            [this](const PropertyList &properties) -> ReturnValue
            {
                bool ok = ac7065e_transport_.SendPrevTrack();
                ESP_LOGI(TAG, "MCP: PREV_TRACK -> %s", ok ? "OK" : "FAIL");
                return ok;
            });

        // ---- 音量控制 ----

        mcp_server.AddTool("self.ac7065e.volume_up",
            "音量加",
            PropertyList(),
            [this](const PropertyList &properties) -> ReturnValue
            {
                bool ok = ac7065e_transport_.SendVolumeUp();
                ESP_LOGI(TAG, "MCP: VOL_UP -> %s", ok ? "OK" : "FAIL");
                return ok;
            });

        mcp_server.AddTool("self.ac7065e.volume_down",
            "音量减",
            PropertyList(),
            [this](const PropertyList &properties) -> ReturnValue
            {
                bool ok = ac7065e_transport_.SendVolumeDown();
                ESP_LOGI(TAG, "MCP: VOL_DOWN -> %s", ok ? "OK" : "FAIL");
                return ok;
            });

        mcp_server.AddTool("self.ac7065e.volume_set",
            "设置默认音量 (0-100)",
            PropertyList({
                Property("level", kPropertyTypeInteger, 0, 100),
            }),
            [this](const PropertyList &properties) -> ReturnValue
            {
                int level = properties["level"].value<int>();
                if (level < 0) level = 0;
                if (level > 100) level = 100;
                bool ok = ac7065e_transport_.SendVolumeDefault(static_cast<uint8_t>(level));
                ESP_LOGI(TAG, "MCP: VOL_DEFAULT(%d) -> %s", level, ok ? "OK" : "FAIL");
                return ok;
            });

        mcp_server.AddTool("self.ac7065e.volume_max",
            "设置为最大音量",
            PropertyList(),
            [this](const PropertyList &properties) -> ReturnValue
            {
                bool ok = ac7065e_transport_.SendVolumeMax();
                ESP_LOGI(TAG, "MCP: VOL_MAX -> %s", ok ? "OK" : "FAIL");
                return ok;
            });

        mcp_server.AddTool("self.ac7065e.volume_min",
            "设置为最小音量（或静音）",
            PropertyList(),
            [this](const PropertyList &properties) -> ReturnValue
            {
                bool ok = ac7065e_transport_.SendVolumeMin();
                ESP_LOGI(TAG, "MCP: VOL_MIN -> %s", ok ? "OK" : "FAIL");
                return ok;
            });

        // ---- 查询命令 ----

        mcp_server.AddTool("self.ac7065e.get_battery",
            "查询 AC7065E 蓝牙模块电量",
            PropertyList(),
            [this](const PropertyList &properties) -> ReturnValue
            {
                bool ok = ac7065e_transport_.SendGetBattery();
                ESP_LOGI(TAG, "MCP: GET_BATTERY -> %s", ok ? "OK" : "FAIL");
                // 实际电量值通过 AC7065E 异步上报 (CMD_BATTERY_RESP)
                return ok;
            });

        mcp_server.AddTool("self.ac7065e.get_volume",
            "查询 AC7065E 蓝牙模块当前音量",
            PropertyList(),
            [this](const PropertyList &properties) -> ReturnValue
            {
                bool ok = ac7065e_transport_.SendGetVolume();
                ESP_LOGI(TAG, "MCP: GET_VOL -> %s", ok ? "OK" : "FAIL");
                // 实际音量值通过 AC7065E 异步上报 (CMD_VOL_RESP)
                return ok;
            });

        ESP_LOGI(TAG, "AC7065E MCP tools registered");
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
                auto instance = static_cast<FogSeekNanoYingZhi *>(arg);
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

        // 通知 AC7065E 进入AI语音对话模式
        ac7065e_transport_.SendWakeUp();

        ESP_LOGI(TAG, "Device powered on.");

        HandleAutoWake(); // 开机自动唤醒
    }

    // 关机流程
    void PowerOff()
    {
        power_manager_.PowerOff();
        led_controller_.UpdateLedStatus(power_manager_);

        // 通知 AC7065E 切换到蓝牙模式
        ac7065e_transport_.SendSleep();

        Application::GetInstance().SetDeviceState(DeviceState::kDeviceStateIdle); // 关机后将设备状态设置为空闲，便于下次开机自动唤醒

        ESP_LOGI(TAG, "Device powered off.");
    }

public:
    FogSeekNanoYingZhi() : boot_button_(BOOT_BUTTON_GPIO), ctrl_button_(CTRL_BUTTON_GPIO)
    {
        InitializeI2c();
        InitializePowerManager();
        InitializeLedController();
        InitializeAudioAmplifier();
        InitializeButtonCallbacks();
        InitializeAC7065EUart();
        InitializeTools();

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

    ~FogSeekNanoYingZhi()
    {
        if (i2c_bus_)
        {
            i2c_del_master_bus(i2c_bus_);
        }
    }
};

DECLARE_BOARD(FogSeekNanoYingZhi);