#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/rtc_io.h>
#include <esp_log.h>
#include <esp_timer.h>
#include "ac7065e_transport.h"
#include "adc_battery_monitor.h"
#include "application.h"
#include "assets/lang_config.h"
#include "button.h"
#include "codecs/box_audio_codec.h"
#include "config.h"
#include "device_state_machine.h"
#include "lamp_controller.h"
#include "led/single_led.h"
#include "led_controller.h"
#include "mcp_server.h"
#include "power_manager.h"
#include "system_reset.h"
#include "wifi_board.h"

#define TAG "FogSeekEdgeYingZhi"

// 重发配置
#define RETRY_INTERVAL_MS   500   // 重发间隔 (ms)
#define RETRY_MAX_COUNT     3     // 最大重发次数
#define ACK_TIMEOUT_MS      500  // 等待 ACK 超时时间 (ms)

class FogSeekEdgeYingZhi : public WifiBoard {
private:
    Button boot_button_;
    Button next_button_;
    Button prev_button_;
    Button led_switch_button_;
    FogSeekPowerManager power_manager_;
    FogSeekLedController led_controller_;
    AC7065ETransport ac7065e_transport_;

    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    AudioCodec* audio_codec_ = nullptr;
    esp_timer_handle_t check_idle_timer_ = nullptr;
    esp_timer_handle_t wake_word_check_timer_ = nullptr;
    RgbLedStrip* rgb_led_strip_ = nullptr;
    bool rgb_led_on_ = false;

    // AUX 状态标记
    bool aux_inserted_ = false;

    // ---- 重发机制 ----
    // 待确认命令结构
    struct PendingCommand {
        uint8_t cmd;
        uint8_t data_len;
        uint8_t data[8];  // 最多 8 字节数据
        uint8_t retry_count;
        esp_timer_handle_t retry_timer;
        bool (*send_func)(AC7065ETransport*, uint8_t, const uint8_t*, uint8_t);
    };
    PendingCommand pending_cmd_;
    bool has_pending_cmd_ = false;

    // 上一次检测到唤醒词的设备状态，用于防抖
    DeviceState last_wake_word_state_ = kDeviceStateUnknown;

    // 发送命令并启动重发定时器
    void SendCommandWithRetry(uint8_t cmd, const uint8_t* data = nullptr, uint8_t data_len = 0,
                              bool (*send_func)(AC7065ETransport*, uint8_t, const uint8_t*, uint8_t) = nullptr) {
        // 如果上一次命令还没收到 ACK，先取消旧的重发
        CancelPendingRetry();

        // 执行发送
        bool sent = false;
        if (send_func) {
            sent = send_func(&ac7065e_transport_, cmd, data, data_len);
        } else {
            if (data_len == 0) {
                sent = ac7065e_transport_.SendCommand(cmd);
            } else if (data_len == 1) {
                sent = ac7065e_transport_.SendCommand(cmd, data[0]);
            } else {
                sent = ac7065e_transport_.SendCommand(cmd, data, data_len);
            }
        }

        if (!sent) {
            ESP_LOGW(TAG, "Failed to send cmd=0x%02X, will retry", cmd);
        }

        // 保存待确认命令
        pending_cmd_.cmd = cmd;
        pending_cmd_.data_len = data_len;
        if (data && data_len > 0 && data_len <= sizeof(pending_cmd_.data)) {
            memcpy(pending_cmd_.data, data, data_len);
        }
        pending_cmd_.retry_count = 0;
        pending_cmd_.send_func = send_func;
        has_pending_cmd_ = true;

        // 启动重发定时器
        StartRetryTimer();
    }

    // 启动重发定时器
    void StartRetryTimer() {
        if (pending_cmd_.retry_timer) {
            esp_timer_stop(pending_cmd_.retry_timer);
            esp_timer_delete(pending_cmd_.retry_timer);
            pending_cmd_.retry_timer = nullptr;
        }

        esp_timer_create_args_t timer_args = {};
        timer_args.callback = [](void* arg) {
            auto self = static_cast<FogSeekEdgeYingZhi*>(arg);
            self->OnRetryTimer();
        };
        timer_args.arg = this;
        timer_args.name = "ac7065e_retry";

        esp_timer_create(&timer_args, &pending_cmd_.retry_timer);
        esp_timer_start_once(pending_cmd_.retry_timer, RETRY_INTERVAL_MS * 1000);
    }

    // 重发定时器回调
    void OnRetryTimer() {
        if (!has_pending_cmd_) return;

        pending_cmd_.retry_count++;
        ESP_LOGW(TAG, "Retry #%d for cmd=0x%02X", pending_cmd_.retry_count, pending_cmd_.cmd);

        bool sent = false;
        if (pending_cmd_.send_func) {
            sent = pending_cmd_.send_func(&ac7065e_transport_, pending_cmd_.cmd,
                                          pending_cmd_.data, pending_cmd_.data_len);
        } else {
            if (pending_cmd_.data_len == 0) {
                sent = ac7065e_transport_.SendCommand(pending_cmd_.cmd);
            } else if (pending_cmd_.data_len == 1) {
                sent = ac7065e_transport_.SendCommand(pending_cmd_.cmd, pending_cmd_.data[0]);
            } else {
                sent = ac7065e_transport_.SendCommand(pending_cmd_.cmd,
                                                      pending_cmd_.data, pending_cmd_.data_len);
            }
        }

        if (pending_cmd_.retry_count < RETRY_MAX_COUNT) {
            // 还有重试次数，继续等待
            StartRetryTimer();
        } else {
            // 超过最大重试次数，放弃
            ESP_LOGE(TAG, "Max retries reached for cmd=0x%02X, giving up", pending_cmd_.cmd);
            CancelPendingRetry();
        }
    }

    // 处理收到的 ACK
    void HandleAck(uint8_t acked_cmd) {
        if (has_pending_cmd_ && pending_cmd_.cmd == acked_cmd) {
            ESP_LOGI(TAG, "ACK received for cmd=0x%02X, retry cleared", acked_cmd);
            CancelPendingRetry();
        }
    }

    // 取消待确认命令和定时器
    void CancelPendingRetry() {
        if (pending_cmd_.retry_timer) {
            esp_timer_stop(pending_cmd_.retry_timer);
            esp_timer_delete(pending_cmd_.retry_timer);
            pending_cmd_.retry_timer = nullptr;
        }
        has_pending_cmd_ = false;
    }

    // 唤醒词检测回调：设备状态变为 kDeviceStateConnecting 时发送唤醒指令
    void OnWakeWordTriggered() {
        ESP_LOGI(TAG, "Wake word detected! Sending WAKEUP to AC7065E...");
        SendCommandWithRetry(CMD_WAKEUP);
    }

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

    // 初始化按键回调
    void InitializeButtonCallbacks() {
        // IO40_LED: 单击打断对话，双击配网，长按开关RGB灯
        led_switch_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            app.ToggleChatState();  // 切换聊天状态（打断）
        });
        led_switch_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
        });
        led_switch_button_.OnLongPress([this]() {
            if (rgb_led_strip_ == nullptr) {
                ESP_LOGW(TAG, "RGB LED not initialized");
                return;
            }
            rgb_led_on_ = !rgb_led_on_;
            if (rgb_led_on_) {
                rgb_led_strip_->SetAllColor({255, 255, 255});  // 白色
                ESP_LOGI(TAG, "RGB LED ON");
            } else {
                rgb_led_strip_->SetAllColor({0, 0, 0});  // 关闭
                ESP_LOGI(TAG, "RGB LED OFF");
            }
        });

        // IO47_NEXT/V+: 单击下一首，长按增大音量
        next_button_.OnClick([this]() {
            SendCommandWithRetry(CMD_NEXT_TRACK);
            ESP_LOGI(TAG, "Next track");
        });
        next_button_.OnLongPress([this]() {
            SendCommandWithRetry(CMD_VOL_UP);
            ESP_LOGI(TAG, "Volume up");
        });

        // IO39_PREV/V-: 单击上一首，长按减小音量
        prev_button_.OnClick([this]() {
            SendCommandWithRetry(CMD_PREV_TRACK);
            ESP_LOGI(TAG, "Previous track");
        });
        prev_button_.OnLongPress([this]() {
            SendCommandWithRetry(CMD_VOL_DOWN);
            ESP_LOGI(TAG, "Volume down");
        });
    }

    // 初始化 RGB LED（直接使用 RgbLedStrip，不需要红绿灯）
    void InitializeLedController() {
        rgb_led_strip_ = new RgbLedStrip(LED_RGB_GPIO, LED_RGB_NUM_LEDS);
        ESP_LOGI(TAG, "RGB LED initialized on GPIO%d, %d LEDs", LED_RGB_GPIO, LED_RGB_NUM_LEDS);
    }

    // 初始化 AC7065E UART 通信
    void InitializeAC7065EUart() {
        ESP_LOGI(TAG, "Initializing AC7065E UART...");

        bool init_result = ac7065e_transport_.Initialize(
            UART_NUM_1, AC7065E_UART_TX_PIN, AC7065E_UART_RX_PIN, AC7065E_UART_BAUD_RATE);
        ESP_LOGI(TAG, "AC7065E UART init result: %s", init_result ? "SUCCESS" : "FAILED");

        vTaskDelay(pdMS_TO_TICKS(200));

        ESP_LOGI(TAG, "Starting AC7065E receive task...");
        ac7065e_transport_.StartReceiveTask([this](uint8_t cmd, const uint8_t* data, uint8_t len) {
            HandleAC7065EMessage(cmd, data, len);
        });

        // 启动唤醒词检测定时器 (每 200ms 检查一次设备状态变化)
        StartWakeWordCheckTimer();
    }

    // 启动唤醒词检测定时器
    void StartWakeWordCheckTimer() {
        esp_timer_create_args_t timer_args = {};
        timer_args.callback = [](void* arg) {
            auto self = static_cast<FogSeekEdgeYingZhi*>(arg);
            self->CheckWakeWordState();
        };
        timer_args.arg = this;
        timer_args.name = "wake_word_check";
        esp_timer_create(&timer_args, &wake_word_check_timer_);
        esp_timer_start_periodic(wake_word_check_timer_, 200000);  // 200ms
    }

    // 周期性检查唤醒词触发状态
    void CheckWakeWordState() {
        auto& app = Application::GetInstance();
        auto state = app.GetDeviceState();

        // 检测状态从 Idle/Unknown 变为 Connecting/Listening (唤醒词触发)
        if ((last_wake_word_state_ == kDeviceStateIdle || last_wake_word_state_ == kDeviceStateUnknown) &&
            (state == kDeviceStateConnecting || state == kDeviceStateListening)) {
            ESP_LOGI(TAG, "Wake word triggered! state: %d -> %d, sending WAKEUP",
                     (int)last_wake_word_state_, (int)state);
            SendCommandWithRetry(CMD_WAKEUP);
        }

        last_wake_word_state_ = state;
    }

    // 处理 AC7065E 上报消息
    void HandleAC7065EMessage(uint8_t cmd, const uint8_t* data, uint8_t len) {
        ESP_LOGI(TAG, "AC7065E message: cmd=0x%02X, len=%d", cmd, len);

        switch (cmd) {
            case CMD_POWER_ON:  // 0x80 - 开关机状态上报
                if (len >= 1) {
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
                if (len >= 1) {
                    ESP_LOGI(TAG, "AC7065E ACK for cmd: 0x%02X", data[0]);
                    HandleAck(data[0]);
                }
                break;

            case CMD_BATTERY_RESP:  // 0x84 - 电量响应
                if (len >= 1) {
                    ESP_LOGI(TAG, "AC7065E battery level: %d%%", data[0]);
                }
                break;

            case CMD_VOL_RESP:  // 0x85 - 音量响应
                if (len >= 1) {
                    ESP_LOGI(TAG, "AC7065E volume level: %d%%", data[0]);
                }
                break;

            case CMD_ERROR:  // 0xFF - 错误
                if (len >= 2) {
                    ESP_LOGW(TAG, "AC7065E error: cmd=0x%02X, error_type=0x%02X", data[0], data[1]);
                }
                break;

            default:
                ESP_LOGW(TAG, "Unknown AC7065E command: 0x%02X", cmd);
                break;
        }
    }

    // AUX 插入处理: 关闭AI语音音频和功放
    void HandleAuxInserted() {
        auto& app = Application::GetInstance();

        // 如果正在对话中，结束对话
        if (app.GetDeviceState() == kDeviceStateListening ||
            app.GetDeviceState() == kDeviceStateSpeaking) {
            app.SetDeviceState(kDeviceStateIdle);
        }

        ESP_LOGI(TAG, "AUX mode: AI audio disabled, amplifier off");
    }

    // AUX 拔出处理: 恢复AI功能
    void HandleAuxRemoved() { ESP_LOGI(TAG, "AUX removed: AI audio restored, amplifier on"); }

    // ============================================================
    // MCP 工具注册 (AI 可通过调用接口下发 AC7065E 命令)
    // ============================================================
    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();

        // ---- 唤醒/休眠控制 ----

        mcp_server.AddTool("self.ac7065e.wakeup", "使 AC7065E 蓝牙音频协处理器进入 AI 语音对话模式，对话模式同样保持蓝牙音乐播放",
                           PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                               SendCommandWithRetry(CMD_WAKEUP);
                               ESP_LOGI(TAG, "MCP: WAKEUP -> OK");
                               return true;
                           });

        mcp_server.AddTool("self.ac7065e.sleep", "关闭 AI 语音对话模式，切换回蓝牙模式，蓝牙模式同样可以进行指令下发，当用户说退下/再见以及长时间无对话时，在进入待命状态前先切换到蓝牙模式再进入待命",
                           PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                               SendCommandWithRetry(CMD_SLEEP);
                               ESP_LOGI(TAG, "MCP: SLEEP -> OK");
                               return true;
                           });

        // ---- 音源切换 ----

        mcp_server.AddTool("self.ac7065e.play", "恢复蓝牙音乐播放", PropertyList(),
                           [this](const PropertyList& properties) -> ReturnValue {
                               SendCommandWithRetry(CMD_PLAY);
                               ESP_LOGI(TAG, "MCP: PLAY -> OK");
                               return true;
                           });

        mcp_server.AddTool("self.ac7065e.pause", "暂停蓝牙音乐", PropertyList(),
                           [this](const PropertyList& properties) -> ReturnValue {
                               SendCommandWithRetry(CMD_PAUSE);
                               ESP_LOGI(TAG, "MCP: PAUSE -> OK");
                               return true;
                           });

        mcp_server.AddTool("self.ac7065e.next_track", "切换到下一曲", PropertyList(),
                           [this](const PropertyList& properties) -> ReturnValue {
                               SendCommandWithRetry(CMD_NEXT_TRACK);
                               ESP_LOGI(TAG, "MCP: NEXT_TRACK -> OK");
                               return true;
                           });

        mcp_server.AddTool("self.ac7065e.prev_track", "切换到上一曲", PropertyList(),
                           [this](const PropertyList& properties) -> ReturnValue {
                               SendCommandWithRetry(CMD_PREV_TRACK);
                               ESP_LOGI(TAG, "MCP: PREV_TRACK -> OK");
                               return true;
                           });

        // ---- 音量控制 ----

        mcp_server.AddTool("self.ac7065e.volume_up", "音量加", PropertyList(),
                           [this](const PropertyList& properties) -> ReturnValue {
                               SendCommandWithRetry(CMD_VOL_UP);
                               ESP_LOGI(TAG, "MCP: VOL_UP -> OK");
                               return true;
                           });

        mcp_server.AddTool("self.ac7065e.volume_down", "音量减", PropertyList(),
                           [this](const PropertyList& properties) -> ReturnValue {
                               SendCommandWithRetry(CMD_VOL_DOWN);
                               ESP_LOGI(TAG, "MCP: VOL_DOWN -> OK");
                               return true;
                           });

        mcp_server.AddTool(
            "self.ac7065e.volume_set", "设置默认音量 (0-100)",
            PropertyList({
                Property("level", kPropertyTypeInteger, 0, 100),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int level = properties["level"].value<int>();
                if (level < 0)
                    level = 0;
                if (level > 100)
                    level = 100;
                uint8_t vol = static_cast<uint8_t>(level);
                SendCommandWithRetry(CMD_VOL_DEFAULT, &vol, 1);
                ESP_LOGI(TAG, "MCP: VOL_DEFAULT(%d) -> OK", level);
                return true;
            });

        mcp_server.AddTool("self.ac7065e.volume_max", "设置为最大音量", PropertyList(),
                           [this](const PropertyList& properties) -> ReturnValue {
                               SendCommandWithRetry(CMD_VOL_MAX);
                               ESP_LOGI(TAG, "MCP: VOL_MAX -> OK");
                               return true;
                           });

        mcp_server.AddTool("self.ac7065e.volume_min", "设置为最小音量（或静音）", PropertyList(),
                           [this](const PropertyList& properties) -> ReturnValue {
                               SendCommandWithRetry(CMD_VOL_MIN);
                               ESP_LOGI(TAG, "MCP: VOL_MIN -> OK");
                               return true;
                           });

        // ---- 查询命令 ----

        mcp_server.AddTool("self.ac7065e.get_battery", "查询 AC7065E 蓝牙模块电量", PropertyList(),
                           [this](const PropertyList& properties) -> ReturnValue {
                               SendCommandWithRetry(CMD_GET_BATTERY);
                               ESP_LOGI(TAG, "MCP: GET_BATTERY -> OK");
                               return true;
                           });

        mcp_server.AddTool("self.ac7065e.get_volume", "查询 AC7065E 蓝牙模块当前音量",
                           PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                               SendCommandWithRetry(CMD_GET_VOL);
                               ESP_LOGI(TAG, "MCP: GET_VOL -> OK");
                               return true;
                           });

        ESP_LOGI(TAG, "AC7065E MCP tools registered");
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
                auto instance = static_cast<FogSeekEdgeYingZhi*>(arg);
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
        // 通知 AC7065E 进入AI语音对话模式
        SendCommandWithRetry(CMD_WAKEUP);

        ESP_LOGI(TAG, "Device powered on.");

        HandleAutoWake();  // 开机自动唤醒
    }

    // 关机流程
    void PowerOff() {
        // 通知 AC7065E 切换到蓝牙模式
        SendCommandWithRetry(CMD_SLEEP);

        Application::GetInstance().SetDeviceState(
            DeviceState::kDeviceStateIdle);  // 关机后将设备状态设置为空闲，便于下次开机自动唤醒

        ESP_LOGI(TAG, "Device powered off.");
    }

public:
    FogSeekEdgeYingZhi()
        : boot_button_(BOOT_BUTTON_GPIO),
          next_button_(NEXT_BUTTON_GPIO),
          prev_button_(PREV_BUTTON_GPIO),
          led_switch_button_(LED_SWITCH_GPIO) {
        InitializeI2c();
        InitializeLedController();
        InitializeButtonCallbacks();
        InitializeAC7065EUart();
        InitializeTools();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    ~FogSeekEdgeYingZhi() {
        CancelPendingRetry();
        if (wake_word_check_timer_) {
            esp_timer_stop(wake_word_check_timer_);
            esp_timer_delete(wake_word_check_timer_);
            wake_word_check_timer_ = nullptr;
        }
        if (rgb_led_strip_) {
            delete rgb_led_strip_;
        }
        if (i2c_bus_) {
            i2c_del_master_bus(i2c_bus_);
        }
    }
};

DECLARE_BOARD(FogSeekEdgeYingZhi);