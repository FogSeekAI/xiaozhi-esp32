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
#define RETRY_MAX_COUNT     1     // 最大重发次数
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
    int state_listener_id_ = -1;  // 状态变化监听器 ID
    RgbLedStrip* rgb_led_strip_ = nullptr;
    bool rgb_led_on_ = false;

    // RGB 模式颜色（由 AI 通过 MCP 工具 self.rgb.set_color 控制）
    StripColor rgb_off_color_ = {0, 0, 0};         // 关闭

    // 运行模式标记
    bool ai_mode_ = false;        // 开机默认进入蓝牙模式

    // AUX 状态标记
    bool aux_inserted_ = false;

    // ---- 查询响应同步 ----
    SemaphoreHandle_t query_semaphore_ = nullptr;  // get_battery/get_volume 同步等待信号量
    int queried_value_ = -1;                        // 查询结果缓存

    // ---- 重发机制 ----
    // 待确认命令结构
    struct PendingCommand {
        uint8_t cmd = 0;
        uint8_t data_len = 0;
        uint8_t data[8] = {};  // 最多 8 字节数据
        uint8_t retry_count = 0;
        esp_timer_handle_t retry_timer = nullptr;
        bool (*send_func)(AC7065ETransport*, uint8_t, const uint8_t*, uint8_t) = nullptr;
    };
    PendingCommand pending_cmd_;
    bool has_pending_cmd_ = false;

    // 发送命令 (重发机制暂时注释掉用于测试)
    void SendCommandWithRetry(uint8_t cmd, const uint8_t* data = nullptr, uint8_t data_len = 0,
                              bool (*send_func)(AC7065ETransport*, uint8_t, const uint8_t*, uint8_t) = nullptr) {
        // // 如果上一次命令还没收到 ACK，先取消旧的重发
        // CancelPendingRetry();

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
            ESP_LOGW(TAG, "Failed to send cmd=0x%02X", cmd);
        }

        // // 保存待确认命令
        // pending_cmd_.cmd = cmd;
        // pending_cmd_.data_len = data_len;
        // if (data && data_len > 0 && data_len <= sizeof(pending_cmd_.data)) {
        //     memcpy(pending_cmd_.data, data, data_len);
        // }
        // pending_cmd_.retry_count = 0;
        // pending_cmd_.send_func = send_func;
        // has_pending_cmd_ = true;

        // // 启动重发定时器
        // StartRetryTimer();
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

    // 根据当前运行模式和设备状态更新 RGB LED 效果
    // AI 模式用红色，蓝牙模式用蓝色；两种模式都根据设备状态变换灯光效果
    void UpdateLedByState(DeviceState state) {
        if (!rgb_led_strip_) return;

        StripColor color = ai_mode_ ? StripColor{255, 0, 0} : StripColor{0, 0, 255};

        switch (state) {
            case kDeviceStateSpeaking:
                rgb_led_strip_->Blink(color, 500);      // 说话时闪烁
                break;
            case kDeviceStateIdle:
                rgb_led_strip_->StartBreathe(3000);     // 待命时呼吸灯
                break;
            case kDeviceStateListening:
            case kDeviceStateConnecting:
                rgb_led_strip_->SetAllColor(color);     // 聆听/连接时常亮
                break;
            default:
                rgb_led_strip_->SetAllColor(color);     // 其他状态常亮
                break;
        }
        rgb_led_on_ = true;
    }

    // 注册状态变化监听器，实时响应唤醒词检测
    void RegisterStateChangeListener() {
        auto& app = Application::GetInstance();
        auto& state_machine = app.GetStateMachine();
        state_listener_id_ = state_machine.AddStateChangeListener(
            [this](DeviceState old_state, DeviceState new_state) {
                auto& app = Application::GetInstance();

                // ---- 唤醒词检测：仅在唤醒词触发时发送 WAKEUP ----
                if (app.IsWakeWordTriggered()) {
                    // 覆盖所有唤醒词触发的状态路径：
                    //   Idle -> Connecting   (首次唤醒，开启音频通道)
                    //   Idle -> Listening    (通道已开，直接监听)
                    //   Speaking -> Listening (打断说话)
                    //   Listening -> Listening(重听)
                    if (new_state == kDeviceStateConnecting || new_state == kDeviceStateListening) {
                        ESP_LOGI(TAG, "Wake word triggered! state: %d -> %d, scheduling WAKEUP",
                                 (int)old_state, (int)new_state);
                        ai_mode_ = true;  // 进入 AI 模式
                        app.Schedule([this]() {
                            SendCommandWithRetry(CMD_WAKEUP);
                        });
                    }
                }

                // ---- 根据当前状态更新 RGB LED 效果 ----
                UpdateLedByState(new_state);
            });
        ESP_LOGI(TAG, "State change listener registered (id=%d) for state-based LED control",
                 state_listener_id_);
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

        // 创建查询响应同步信号量
        query_semaphore_ = xSemaphoreCreateBinary();
        ESP_LOGI(TAG, "Query semaphore created");

        bool init_result = ac7065e_transport_.Initialize(
            UART_NUM_1, AC7065E_UART_TX_PIN, AC7065E_UART_RX_PIN, AC7065E_UART_BAUD_RATE);
        ESP_LOGI(TAG, "AC7065E UART init result: %s", init_result ? "SUCCESS" : "FAILED");

        vTaskDelay(pdMS_TO_TICKS(200));

        ESP_LOGI(TAG, "Starting AC7065E receive task...");
        ac7065e_transport_.StartReceiveTask([this](uint8_t cmd, const uint8_t* data, uint8_t len) {
            HandleAC7065EMessage(cmd, data, len);
        });

        // 注册状态变化监听器：当检测到唤醒词时立即发送唤醒指令（零延迟）
        RegisterStateChangeListener();
    }

    // 处理 AC7065E 上报消息
    void HandleAC7065EMessage(uint8_t cmd, const uint8_t* data, uint8_t len) {
        ESP_LOGI(TAG, "AC7065E message: cmd=0x%02X, len=%d", cmd, len);

        // AC7065E 回显应答: cmd < 0x80 表示对方回传了我们发送的指令作为 ACK
        if (cmd < 0x80) {
            ESP_LOGI(TAG, "AC7065E ACK echo for cmd: 0x%02X", cmd);
            // HandleAck(cmd);  // 重发机制暂时注释掉
            return;
        }

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

            case CMD_BATTERY_RESP:  // 0x84 - 电量响应
                if (len >= 1) {
                    ESP_LOGI(TAG, "AC7065E battery level: %d%%", data[0]);
                    queried_value_ = data[0];
                    if (query_semaphore_) xSemaphoreGive(query_semaphore_);
                }
                break;

            case CMD_VOL_RESP:  // 0x85 - 音量响应
                if (len >= 1) {
                    ESP_LOGI(TAG, "AC7065E volume level: %d%%", data[0]);
                    queried_value_ = data[0];
                    if (query_semaphore_) xSemaphoreGive(query_semaphore_);
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
    // MCP 工具注册 (AI 可通过调用接口下发 AC7065E 命令及 RGB 灯控制)
    // ============================================================
    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();

        // ---- 模式切换 ----

        mcp_server.AddTool("self.ac7065e.switch_to_ai", 
                           "切换至 AI 语音对话模式。"
                           "当用户表示想要聊天时，如果在蓝牙模式下，先调用这个工具进入AI语音对话模式，因为只有在AI语音对话模式下，才能进行对话。",
                           PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                               ai_mode_ = true;
                               SendCommandWithRetry(CMD_WAKEUP);
                               // 自动同步 RGB（AI 模式：红色）
                               if (rgb_led_strip_) {
                                   StripColor red = {255, 0, 0};
                                   rgb_led_strip_->SetAllColor(red);
                                   rgb_led_on_ = true;
                                   ESP_LOGI(TAG, "MCP: WAKEUP -> OK, RGB set to red (AI mode)");
                               } else {
                                   ESP_LOGI(TAG, "MCP: WAKEUP -> OK");
                               }
                               return true;
                           });

        mcp_server.AddTool("self.ac7065e.switch_to_bluetooth", 
                           "关闭 AI 语音对话模式，切换回蓝牙模式。蓝牙模式下 RGB 灯变为蓝色，同样会根据设备状态变换效果："
                           "说话时闪烁、待命时呼吸灯、聆听时常亮。"
                           
                           "蓝牙模式下同样可以进行音乐播放、音量调节等指令下发。",
                           PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                               ai_mode_ = false;
                               SendCommandWithRetry(CMD_SLEEP);
                               // 自动同步 RGB 为蓝色（蓝牙模式）
                               if (rgb_led_strip_) {
                                   StripColor blue = {0, 0, 255};
                                   rgb_led_strip_->SetAllColor(blue);
                                   rgb_led_on_ = true;
                                   ESP_LOGI(TAG, "MCP: SLEEP -> OK, RGB set to blue (BT mode)");
                               } else {
                                   ESP_LOGI(TAG, "MCP: SLEEP -> OK");
                               }
                               return true;
                           });

        // ---- 音源切换 ----

        mcp_server.AddTool("self.ac7065e.play", 
                           "恢复蓝牙音乐播放。\n"
                           "【重要】用户说\"播放音乐\"、\"播放蓝牙音乐\"、\"放首歌\"等指令时，请按以下两步顺序调用：\n"
                           "  1. 先调用 self.ac7065e.switch_to_bluetooth 切换到蓝牙模式\n"
                           "  2. 等待确认切换成功（收到返回 true 后），再调用此工具播放音乐\n"
                           "不要跳过第一步直接播放，因为 AI 模式下音频通道被占用，无法播放蓝牙音乐。",
                           PropertyList(),
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

        mcp_server.AddTool("self.ac7065e.volume_up", "蓝牙音量增加一格（固定步进，约10%）", PropertyList(),
                           [this](const PropertyList& properties) -> ReturnValue {
                               SendCommandWithRetry(CMD_VOL_UP);
                               ESP_LOGI(TAG, "MCP: VOL_UP -> OK");
                               return true;
                           });

        mcp_server.AddTool("self.ac7065e.volume_down", "蓝牙音量减少一格（固定步进，约10%）", PropertyList(),
                           [this](const PropertyList& properties) -> ReturnValue {
                               SendCommandWithRetry(CMD_VOL_DOWN);
                               ESP_LOGI(TAG, "MCP: VOL_DOWN -> OK");
                               return true;
                           });

        mcp_server.AddTool(
            "self.ac7065e.volume_set",
            "设置蓝牙音量到指定百分比。参数 level 为 0-100 的整数，0=静音, 50=中等, 100=最大。"
            "当用户说\"音量调到50%\"或\"音量设为80\"时，直接传入对应的数字即可。"
            "注意：这是精确设置，与 volume_up/volume_down 的步进调节不同",
            PropertyList({
                Property("level", kPropertyTypeInteger, 0, 100),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int level = properties["level"].value<int>();
                if (level < 0)
                    level = 0;
                if (level > 100)
                    level = 100;
                ac7065e_transport_.SendVolumeDefault(static_cast<uint8_t>(level));
                ESP_LOGI(TAG, "MCP: VOL_DEFAULT(%d) -> OK", level);
                return true;
            });

        mcp_server.AddTool("self.ac7065e.volume_max", "蓝牙音量设置为最大（实际范围上限90%）", PropertyList(),
                           [this](const PropertyList& properties) -> ReturnValue {
                               ac7065e_transport_.SendVolumeDefault(90);
                               ESP_LOGI(TAG, "MCP: VOL_MAX(90%%) -> OK");
                               return true;
                           });

        mcp_server.AddTool("self.ac7065e.volume_min", "蓝牙音量设置为最小/静音（0%）", PropertyList(),
                           [this](const PropertyList& properties) -> ReturnValue {
                               ac7065e_transport_.SendVolumeDefault(0);
                               ESP_LOGI(TAG, "MCP: VOL_MIN(0%%) -> OK");
                               return true;
                           });

        // ---- 查询命令 ----

        mcp_server.AddTool("self.ac7065e.get_battery", "查询 AC7065E 蓝牙模块电量",
                           PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                               queried_value_ = -1;
                               // 清空可能残留的信号量（上次超时延迟响应）
                               if (query_semaphore_) xSemaphoreTake(query_semaphore_, 0);
                               SendCommandWithRetry(CMD_GET_BATTERY);
                               if (query_semaphore_ && xSemaphoreTake(query_semaphore_, pdMS_TO_TICKS(500)) == pdTRUE) {
                                   ESP_LOGI(TAG, "MCP: GET_BATTERY -> %d%%", queried_value_);
                                   return queried_value_;
                               }
                               ESP_LOGW(TAG, "MCP: GET_BATTERY timeout");
                               return std::string("查询超时，请重试");
                           });

        mcp_server.AddTool("self.ac7065e.get_volume", "查询 AC7065E 蓝牙模块当前音量",
                           PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                               queried_value_ = -1;
                               if (query_semaphore_) xSemaphoreTake(query_semaphore_, 0);
                               SendCommandWithRetry(CMD_GET_VOL);
                               if (query_semaphore_ && xSemaphoreTake(query_semaphore_, pdMS_TO_TICKS(500)) == pdTRUE) {
                                   ESP_LOGI(TAG, "MCP: GET_VOL -> %d%%", queried_value_);
                                   return queried_value_;
                               }
                               ESP_LOGW(TAG, "MCP: GET_VOL timeout");
                               return std::string("查询超时，请重试");
                           });

        // ============================================================
        // RGB 灯带 MCP 工具
        // ============================================================

        mcp_server.AddTool("self.rgb.on", "打开 RGB 灯带（需要先调用 self.rgb.set_color 设置颜色，或配合 switch_to_ai/switch_to_bluetooth 使用）",
                           PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                               if (!rgb_led_strip_) {
                                   ESP_LOGW(TAG, "RGB LED not initialized");
                                   return false;
                               }
                               rgb_led_on_ = true;
                               ESP_LOGI(TAG, "MCP: RGB ON");
                               return true;
                           });

        mcp_server.AddTool("self.rgb.off", "关闭 RGB 灯带",
                           PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                               if (!rgb_led_strip_) {
                                   ESP_LOGW(TAG, "RGB LED not initialized");
                                   return false;
                               }
                               rgb_led_on_ = false;
                               rgb_led_strip_->SetAllColor(rgb_off_color_);
                               ESP_LOGI(TAG, "MCP: RGB OFF");
                               return true;
                           });

        mcp_server.AddTool(
            "self.rgb.set_color", "设置 RGB 灯带为自定义颜色并自动开灯。用于用户指定颜色场景（如红色、粉色等）。模式切换（AI/蓝牙）时颜色会自动同步，无需手动调用。",
            PropertyList({
                Property("red", kPropertyTypeInteger, 0, 255),
                Property("green", kPropertyTypeInteger, 0, 255),
                Property("blue", kPropertyTypeInteger, 0, 255),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                if (!rgb_led_strip_) {
                    ESP_LOGW(TAG, "RGB LED not initialized");
                    return false;
                }
                int r = properties["red"].value<int>();
                int g = properties["green"].value<int>();
                int b = properties["blue"].value<int>();
                StripColor color = {static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)};
                rgb_led_strip_->SetAllColor(color);
                rgb_led_on_ = true;
                ESP_LOGI(TAG, "MCP: RGB color set to (%d,%d,%d)", r, g, b);
                return true;
            });

        mcp_server.AddTool(
            "self.rgb.set_brightness", "设置 RGB 灯带亮度等级 (1-5)，1=最暗，5=最亮",
            PropertyList({
                Property("level", kPropertyTypeInteger, 1, 5),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                if (!rgb_led_strip_) {
                    ESP_LOGW(TAG, "RGB LED not initialized");
                    return false;
                }
                int level = properties["level"].value<int>();
                if (level < 1) level = 1;
                if (level > 5) level = 5;
                // 通过 IncreaseBrightness/DecreaseBrightness 调整到目标亮度
                for (int i = 0; i < 5; i++) rgb_led_strip_->DecreaseBrightness(); // 先降到最低
                for (int i = 1; i < level; i++) rgb_led_strip_->IncreaseBrightness(); // 再升到目标
                ESP_LOGI(TAG, "MCP: RGB brightness set to %d", level);
                return true;
            });

        mcp_server.AddTool("self.rgb.breath", "RGB 灯带呼吸效果（指定秒数）",
                           PropertyList({
                               Property("seconds", kPropertyTypeInteger, 1, 30),
                           }),
                           [this](const PropertyList& properties) -> ReturnValue {
                               if (!rgb_led_strip_) {
                                   ESP_LOGW(TAG, "RGB LED not initialized");
                                   return false;
                               }
                               int seconds = properties["seconds"].value<int>();
                               if (seconds < 1) seconds = 1;
                               if (seconds > 30) seconds = 30;
                               rgb_led_on_ = true;
                               rgb_led_strip_->StartBreathe(seconds * 1000);
                               ESP_LOGI(TAG, "MCP: RGB breathing for %ds", seconds);
                               return true;
                           });

        mcp_server.AddTool("self.rgb.blink", "RGB 灯带闪烁效果",
                           PropertyList({
                               Property("red", kPropertyTypeInteger, 0, 255),
                               Property("green", kPropertyTypeInteger, 0, 255),
                               Property("blue", kPropertyTypeInteger, 0, 255),
                               Property("interval_ms", kPropertyTypeInteger, 100, 2000),
                           }),
                           [this](const PropertyList& properties) -> ReturnValue {
                               if (!rgb_led_strip_) {
                                   ESP_LOGW(TAG, "RGB LED not initialized");
                                   return false;
                               }
                               int r = properties["red"].value<int>();
                               int g = properties["green"].value<int>();
                               int b = properties["blue"].value<int>();
                               int interval = properties["interval_ms"].value<int>();
                               StripColor color = {static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)};
                               rgb_led_strip_->Blink(color, interval);
                               rgb_led_on_ = true;
                               ESP_LOGI(TAG, "MCP: RGB blinking (%d,%d,%d) every %dms", r, g, b, interval);
                               return true;
                           });

        ESP_LOGI(TAG, "AC7065E + RGB MCP tools registered");
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

    // 开机流程（蓝牙音箱按键开机触发，不改变 RGB 模式）
    void PowerOn() {
        // 通知 AC7065E 进入AI语音对话模式
        SendCommandWithRetry(CMD_WAKEUP);

        ESP_LOGI(TAG, "Device powered on.");

        HandleAutoWake();  // 开机自动唤醒
    }

    // 关机流程（蓝牙音箱按键关机触发）
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

    // 处理语音命令（本地执行，不走 AI 模型，保证快速准确）
    // command_id 对应 Kconfig 中的 CN_SPEECH_COMMAND_ID<n> 配置槽位
    void OnSpeechCommand(int command_id) override {
        ESP_LOGI(TAG, "Speech command: id=%d", command_id);
        
        switch (command_id) {
            case 0: // 蓝牙模式
                ai_mode_ = false;
                SendCommandWithRetry(CMD_SLEEP);
                if (rgb_led_strip_) {
                    rgb_led_strip_->SetAllColor({0, 0, 255});
                    rgb_led_on_ = true;
                }
                ESP_LOGI(TAG, "VC: switch_bluetooth -> SLEEP, RGB blue");
                break;
            case 1: // 对话模式
                ai_mode_ = true;
                SendCommandWithRetry(CMD_WAKEUP);
                if (rgb_led_strip_) {
                    rgb_led_strip_->SetAllColor({255, 0, 0});
                    rgb_led_on_ = true;
                }
                ESP_LOGI(TAG, "VC: switch_ai -> WAKEUP, RGB red");
                break;
            case 2: // 增大音量
                SendCommandWithRetry(CMD_VOL_UP);
                ESP_LOGI(TAG, "VC: vol_up");
                break;
            case 3: // 减小音量
                SendCommandWithRetry(CMD_VOL_DOWN);
                ESP_LOGI(TAG, "VC: vol_down");
                break;
            case 4: // 下一首
                SendCommandWithRetry(CMD_NEXT_TRACK);
                ESP_LOGI(TAG, "VC: next_track");
                break;
            case 5: // 上一首
                SendCommandWithRetry(CMD_PREV_TRACK);
                ESP_LOGI(TAG, "VC: prev_track");
                break;
            case 6: // 播放音乐
                SendCommandWithRetry(CMD_PLAY);
                ESP_LOGI(TAG, "VC: play");
                break;
            case 7: // 暂停音乐
                SendCommandWithRetry(CMD_PAUSE);
                ESP_LOGI(TAG, "VC: pause");
                break;
            default:
                ESP_LOGW(TAG, "Unknown speech command id: %d", command_id);
                break;
        }
    }

    ~FogSeekEdgeYingZhi() {
        CancelPendingRetry();
        if (query_semaphore_) {
            vSemaphoreDelete(query_semaphore_);
        }
        // 移除状态变化监听器
        if (state_listener_id_ >= 0) {
            Application::GetInstance().GetStateMachine().RemoveStateChangeListener(state_listener_id_);
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