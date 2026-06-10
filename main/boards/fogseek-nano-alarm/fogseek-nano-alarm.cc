#include "wifi_board.h"
#include "config.h"
#include "power_manager.h"
#include "led_controller.h"
#include "codecs/es8389_audio_codec.h"
#include "application.h"
#include "button.h"
#include "mcp_server.h"
#include "assets/lang_config.h"
#include "adc_battery_monitor.h"
#include "device_state_machine.h"
#include "tca6408a_io_expander.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_err.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_io_spi.h>
#include "board.h"
#include "display/lcd_display.h"
#include "lvgl_theme.h"
#include "settings.h"
#include "boards/lilygo-t-circle-s3/esp_lcd_gc9d01n.h"
#include <cJSON.h>
#include <vector>
#include <ctime>

#define TAG "FogSeekNanoAlarm"

// ============================================================================
// 双屏情绪显示封装（与 nano-toy 相同）
// ============================================================================
class DualDisplayEmotionOnly : public Display {
private:
    SemaphoreHandle_t mutex_ = nullptr;

public:
    SpiLcdDisplay* display_1_ = nullptr;
    SpiLcdDisplay* display_2_ = nullptr;

    DualDisplayEmotionOnly(SpiLcdDisplay* disp1, SpiLcdDisplay* disp2)
        : display_1_(disp1), display_2_(disp2) {
        mutex_ = xSemaphoreCreateMutex();
        if (!mutex_) {
            ESP_LOGE(TAG, "Failed to create display mutex!");
        }
    }

    ~DualDisplayEmotionOnly() {
        if (mutex_) {
            vSemaphoreDelete(mutex_);
        }
    }

    void SetEmotion(const char* emotion) override {
        if (display_1_) {
            display_1_->SetEmotion(emotion);
        }
        if (display_2_) {
            display_2_->SetEmotion(emotion);
        }
    }

    void SetTheme(Theme* theme) override {
        if (display_1_) display_1_->SetTheme(theme);
        if (display_2_) display_2_->SetTheme(theme);
    }

    void SetChatMessage(const char* role, const char* content) override {
        if (display_1_) display_1_->SetChatMessage(role, content);
        if (display_2_) display_2_->SetChatMessage(role, content);
    }

    bool Lock(int timeout_ms = 0) override {
        if (!mutex_) return false;
        TickType_t ticks = timeout_ms == 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
        return xSemaphoreTake(mutex_, ticks) == pdTRUE;
    }

    void Unlock() override {
        if (mutex_) {
            xSemaphoreGive(mutex_);
        }
    }
};

// ============================================================================
// 闹钟条目数据结构
// ============================================================================
struct AlarmEntry {
    int id;                 // 闹钟ID（自增）
    time_t trigger_time;    // 触发的 Unix 时间戳
    std::string message;    // 闹钟消息/标签
    bool enabled;
};

// ============================================================================
// FogSeekNanoAlarm 主板类
// ============================================================================
class FogSeekNanoAlarm : public WifiBoard {
private:
    Button boot_button_;
    Button ctrl_button_;
    FogSeekPowerManager power_manager_;
    FogSeekLedController led_controller_;

    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    tca6408a_handle_t tca6408a_handle_;
    AudioCodec* audio_codec_ = nullptr;

    SpiLcdDisplay* display_1_ = nullptr;
    SpiLcdDisplay* display_2_ = nullptr;
    DualDisplayEmotionOnly* dual_display_ = nullptr;

    // --- 闹钟系统 ---
    std::vector<AlarmEntry> alarms_;
    int next_alarm_id_ = 1;
    esp_timer_handle_t alarm_check_timer_ = nullptr;
    bool alarm_active_ = false;
    int active_alarm_id_ = -1;
    esp_timer_handle_t alarm_repeat_timer_ = nullptr;  // 闹钟响铃重复定时器
    int alarm_repeat_count_ = 0;
    static constexpr int ALARM_REPEAT_MAX = 10;        // 最多重复响铃次数
    static constexpr int ALARM_REPEAT_INTERVAL_MS = 5000;  // 重复间隔5秒
    static constexpr int MAX_ALARMS = 20;

    // LED 闹钟闪烁
    esp_timer_handle_t alarm_led_timer_ = nullptr;
    bool alarm_led_state_ = false;

    // ========================================================================
    // 闹钟持久化 (NVS -> JSON)
    // ========================================================================
    void SaveAlarms() {
        // 用 cJSON 将 alarms_ 序列化并存入 NVS
        auto* root = cJSON_CreateArray();
        for (const auto& a : alarms_) {
            auto* obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(obj, "id", a.id);
            cJSON_AddNumberToObject(obj, "trigger_time", (double)a.trigger_time);
            cJSON_AddStringToObject(obj, "message", a.message.c_str());
            cJSON_AddBoolToObject(obj, "enabled", a.enabled ? 1 : 0);
            cJSON_AddItemToArray(root, obj);
        }
        char* json_str = cJSON_PrintUnformatted(root);
        Settings nvs("alarms", true);
        nvs.SetString("alarm_list", json_str);
        cJSON_free(json_str);
        cJSON_Delete(root);
        ESP_LOGI(TAG, "Alarms saved to NVS (%d alarms)", (int)alarms_.size());
    }

    void LoadAlarms() {
        Settings nvs("alarms", false);
        std::string json_str = nvs.GetString("alarm_list", "[]");
        auto* root = cJSON_Parse(json_str.c_str());
        if (!root || !cJSON_IsArray(root)) {
            ESP_LOGI(TAG, "No saved alarms found");
            if (root) cJSON_Delete(root);
            return;
        }

        alarms_.clear();
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, root) {
            AlarmEntry a;
            auto* id = cJSON_GetObjectItem(item, "id");
            auto* trigger = cJSON_GetObjectItem(item, "trigger_time");
            auto* msg = cJSON_GetObjectItem(item, "message");
            auto* en = cJSON_GetObjectItem(item, "enabled");

            a.id = id ? id->valueint : 0;
            a.trigger_time = trigger ? (time_t)trigger->valuedouble : 0;
            a.message = msg ? msg->valuestring : "";
            a.enabled = en ? (en->valueint != 0) : true;

            if (a.id >= next_alarm_id_) {
                next_alarm_id_ = a.id + 1;
            }

            // 清理已过期的闹钟（单次闹钟）
            time_t now = time(NULL);
            if (now > 2025UL * 365 * 24 * 3600 && a.trigger_time < now - 60) {
                ESP_LOGI(TAG, "Pruning expired alarm #%d (triggered at %lld, now %lld)",
                         a.id, (long long)a.trigger_time, (long long)now);
                continue;
            }
            alarms_.push_back(a);
        }
        cJSON_Delete(root);
        ESP_LOGI(TAG, "Loaded %d alarms from NVS", (int)alarms_.size());
    }

    int GetNextAlarmId() {
        return next_alarm_id_++;
    }

    // ========================================================================
    // 闹钟定时检查（每秒一次）
    // ========================================================================
    static void AlarmCheckTimerCallback(void* arg) {
        auto* self = static_cast<FogSeekNanoAlarm*>(arg);
        self->CheckAlarms();
    }

    void CheckAlarms() {
        if (alarm_active_) return;  // 已有闹钟在响，不检查新闹钟

        time_t now = time(NULL);
        // 时间未同步时不触发
        if (now < 2025UL * 365 * 24 * 3600) return;

        for (auto& alarm : alarms_) {
            if (!alarm.enabled) continue;
            if (now >= alarm.trigger_time) {
                TriggerAlarm(alarm);
                alarm.enabled = false;
                SaveAlarms();
                break;  // 一次只触发一个闹钟
            }
        }
    }

    // ========================================================================
    // 触发闹钟
    // ========================================================================
    void TriggerAlarm(const AlarmEntry& alarm) {
        ESP_LOGI(TAG, "================== ALARM FIRED ==================");
        ESP_LOGI(TAG, "Alarm #%d - message: \"%s\"", alarm.id, alarm.message.c_str());

        alarm_active_ = true;
        active_alarm_id_ = alarm.id;
        alarm_repeat_count_ = 0;

        // 1. 振动电机
        StartMotorPulse();

        // 2. LED 红绿交替闪烁
        StartAlarmLedBlink();

        // 3. 播放闹钟音频（使用预置音频）
        Application::GetInstance().PlaySound(Lang::Sounds::OGG_EXCLAMATION);

        // 4. 启动重复响铃定时器
        esp_timer_start_periodic(alarm_repeat_timer_, ALARM_REPEAT_INTERVAL_MS * 1000);

        // 5. 表情显示为闹钟状态
        if (dual_display_) {
            dual_display_->SetEmotion("alarm");
        }

        // 6. 通过 AI 语音播报闹钟
        auto& app = Application::GetInstance();
        std::string wake_msg = alarm.message.empty() ? "闹钟响了" : alarm.message;
        app.Schedule([wake_msg]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.WakeWordInvoke(wake_msg);
            }
        });
    }

    // ========================================================================
    // 关闭闹钟
    // ========================================================================
    void StopAlarm() {
        if (!alarm_active_) return;
        ESP_LOGI(TAG, "Alarm #%d dismissed", active_alarm_id_);

        alarm_active_ = false;
        active_alarm_id_ = -1;

        // 停止电机
        SetMotorState(false);

        // 停止重复定时器
        esp_timer_stop(alarm_repeat_timer_);

        // 停止 LED 闪烁
        StopAlarmLedBlink();

        // 恢复表情
        if (dual_display_) {
            dual_display_->SetEmotion("neutral");
        }
    }

    // ========================================================================
    // 闹钟重复响铃回调
    // ========================================================================
    static void AlarmRepeatTimerCallback(void* arg) {
        auto* self = static_cast<FogSeekNanoAlarm*>(arg);
        if (!self->alarm_active_) return;

        self->alarm_repeat_count_++;
        if (self->alarm_repeat_count_ >= ALARM_REPEAT_MAX) {
            ESP_LOGI(TAG, "Alarm auto-stopped after %d repeats", ALARM_REPEAT_MAX);
            self->StopAlarm();
            return;
        }

        ESP_LOGI(TAG, "Alarm repeat #%d", self->alarm_repeat_count_);
        self->StartMotorPulse();
        // 主线程安全地播放音频
        Application::GetInstance().Schedule([]() {
            Application::GetInstance().PlaySound(Lang::Sounds::OGG_EXCLAMATION);
        });
    }

    // ========================================================================
    // LED 闹钟闪烁
    // ========================================================================
    static void AlarmLedTimerCallback(void* arg) {
        auto* self = static_cast<FogSeekNanoAlarm*>(arg);
        if (!self->alarm_active_) return;

        self->alarm_led_state_ = !self->alarm_led_state_;
        gpio_set_level(LED_RED_GPIO, self->alarm_led_state_ ? 1 : 0);
        gpio_set_level(LED_GREEN_GPIO, self->alarm_led_state_ ? 0 : 1);
    }

    void StartAlarmLedBlink() {
        gpio_set_level(LED_RED_GPIO, 1);
        gpio_set_level(LED_GREEN_GPIO, 0);
        alarm_led_state_ = true;
        esp_timer_start_periodic(alarm_led_timer_, 500000);  // 500ms切换
    }

    void StopAlarmLedBlink() {
        esp_timer_stop(alarm_led_timer_);
        // 恢复LED正常状态
        led_controller_.UpdateLedStatus(power_manager_);
    }

    // ========================================================================
    // 电机控制
    // ========================================================================
    void SetMotorState(bool enable) {
        uint8_t level = enable ? 1 : 0;
        esp_err_t ret = tca6408a_set_gpio_level(&tca6408a_handle_,
                                                static_cast<tca6408a_gpio_t>(TCA6408A_MOTOR_PIN),
                                                level);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set motor state: %d", ret);
            return;
        }
        ESP_LOGI(TAG, "Motor: %s", enable ? "ON" : "OFF");
    }

    void StartMotorPulse() {
        SetMotorState(true);
        // 2秒后自动停止（使用重复闹钟定时器暂不创建额外定时器）
        // 改用一次性定时器的思路：在下一个重复周期前会自动停止
        // 这里直接通过 StopAlarm 统一停止
    }

    // ========================================================================
    // I2C 初始化
    // ========================================================================
    void InitializeI2c() {
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

    void InitializeTca6408a() {
        tca6408a_config_t tca6408a_config = {
            .i2c_bus = i2c_bus_,
            .i2c_address = 0x20,
            .reset_gpio = GPIO_NUM_5};
        esp_err_t ret = tca6408a_init(&tca6408a_handle_, &tca6408a_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize Tca6408a");
            return;
        }
        tca6408a_set_gpio_direction(&tca6408a_handle_, TCA6408A_GPIO_P6, TCA6408A_DIR_OUTPUT);
        tca6408a_set_gpio_level(&tca6408a_handle_, TCA6408A_GPIO_P6, 0);
        ESP_LOGI(TAG, "Tca6408a initialized (motor P6)");
    }

    // ========================================================================
    // 电源管理
    // ========================================================================
    void InitializePowerManager() {
        power_pin_config_t power_pin_config = {
            .hold_gpio = PWR_HOLD_GPIO,
            .charging_gpio = PWR_CHARGING_GPIO,
            .charge_done_gpio = PWR_CHARGE_DONE_GPIO,
            .adc_gpio = BATTERY_ADC_GPIO};
        power_manager_.Initialize(&power_pin_config);
    }

    void InitializeLedController() {
        led_pin_config_t led_pin_config = {
            .red_gpio = LED_RED_GPIO,
            .green_gpio = LED_GREEN_GPIO};
        led_controller_.InitializeLeds(power_manager_, &led_pin_config);
    }

    // ========================================================================
    // 音频功放
    // ========================================================================
    void InitializeAudioAmplifier() {
        gpio_config_t io_conf;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = (1ULL << AUDIO_CODEC_PA_PIN);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        gpio_config(&io_conf);
        SetAudioAmplifierState(false);
    }

    void SetAudioAmplifierState(bool enable) {
        gpio_set_level(AUDIO_CODEC_PA_PIN, enable ? 1 : 0);
    }

    // ========================================================================
    // 按钮回调
    // ========================================================================
    void InitializeButtonCallbacks() {
        // CTRL 按钮：关闭闹钟 / 音量控制
        ctrl_button_.OnClick([this]() {
            // 优先处理闹钟关闭
            if (alarm_active_) {
                StopAlarm();
                ESP_LOGI(TAG, "Alarm dismissed via CTRL button");
                return;
            }
            // 否则控制音量
            auto codec = GetAudioCodec();
            int current_volume = codec->output_volume();
            if (current_volume > 0) {
                codec->SetOutputVolume(0);
                ESP_LOGI(TAG, "Muted - Volume set to 0");
            } else {
                codec->SetOutputVolume(70);
                ESP_LOGI(TAG, "Unmuted - Volume set to 70");
            }
        });

        ctrl_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
        });

        ctrl_button_.OnLongPress([this]() {
            if (!power_manager_.IsPowerOn()) {
                PowerOn();
            } else {
                PowerOff();
            }
        });

        // BOOT 按钮：也用于关闭闹钟
        boot_button_.OnClick([this]() {
            if (alarm_active_) {
                StopAlarm();
                ESP_LOGI(TAG, "Alarm dismissed via BOOT button");
            }
        });
    }

    // ========================================================================
    // 显示初始化
    // ========================================================================
    void InitializeDisplayManager() {
        esp_lcd_panel_io_handle_t panel_io_1 = nullptr;
        esp_lcd_panel_handle_t panel_1 = nullptr;
        esp_lcd_panel_io_handle_t panel_io_2 = nullptr;
        esp_lcd_panel_handle_t panel_2 = nullptr;

        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_SPI_MOSI_GPIO;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SPI_SCLK_GPIO;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

        // Display 1
        esp_lcd_panel_io_spi_config_t io_config_1 = {};
        io_config_1.cs_gpio_num = DISPLAY_SPI_CS_1_GPIO;
        io_config_1.dc_gpio_num = DISPLAY_GC9D01_DC_GPIO;
        io_config_1.spi_mode = 0;
        io_config_1.pclk_hz = 40 * 1000 * 1000;
        io_config_1.trans_queue_depth = 10;
        io_config_1.lcd_cmd_bits = 8;
        io_config_1.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config_1, &panel_io_1));

        esp_lcd_panel_dev_config_t panel_config_1 = {};
        panel_config_1.reset_gpio_num = DISPLAY_GC9D01_RESET_GPIO;
        panel_config_1.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config_1.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9d01n(panel_io_1, &panel_config_1, &panel_1));

        esp_lcd_panel_reset(panel_1);
        esp_lcd_panel_init(panel_1);
        esp_lcd_panel_disp_on_off(panel_1, true);

        display_1_ = new SpiLcdDisplay(panel_io_1, panel_1,
                                       DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                       DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                       DISPLAY_MIRROR_X_1, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);

        // Display 2
        esp_lcd_panel_io_spi_config_t io_config_2 = {};
        io_config_2.cs_gpio_num = DISPLAY_SPI_CS_2_GPIO;
        io_config_2.dc_gpio_num = DISPLAY_GC9D01_DC_GPIO;
        io_config_2.spi_mode = 0;
        io_config_2.pclk_hz = 40 * 1000 * 1000;
        io_config_2.trans_queue_depth = 10;
        io_config_2.lcd_cmd_bits = 8;
        io_config_2.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config_2, &panel_io_2));

        esp_lcd_panel_dev_config_t panel_config_2 = {};
        panel_config_2.reset_gpio_num = GPIO_NUM_NC;
        panel_config_2.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config_2.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9d01n(panel_io_2, &panel_config_2, &panel_2));

        esp_lcd_panel_reset(panel_2);
        esp_lcd_panel_init(panel_2);
        esp_lcd_panel_disp_on_off(panel_2, true);
        esp_lcd_panel_mirror(panel_2, DISPLAY_MIRROR_X_2, DISPLAY_MIRROR_Y);

        display_2_ = new SpiLcdDisplay(panel_io_2, panel_2,
                                       DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                       DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                       DISPLAY_MIRROR_X_2, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);

        if (display_1_ && display_2_) {
            dual_display_ = new DualDisplayEmotionOnly(display_1_, display_2_);
        }
    }

    // ========================================================================
    // MCP 工具注册（AI 对话控制闹钟）
    // ========================================================================
    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();

        // ----- self.alarm.set -----
        // 设置闹钟：支持绝对时间和相对时间两种模式
        mcp_server.AddTool("self.alarm.set",
            "设置一个闹钟。有两种方式：\n"
            "1) 绝对时间 - 指定 hour(0-23) 和 minute(0-59)，在今天的该时刻触发\n"
            "   （如果该时间已过，则明天触发）\n"
            "2) 相对时间 - 指定 seconds_from_now，多少秒后触发\n"
            "参数说明：\n"
            "  type: 'absolute'（绝对时间）或 'relative'（倒计时）\n"
            "  hour: 小时（type=absolute时必填，0-23）\n"
            "  minute: 分钟（type=absolute时必填，0-59）\n"
            "  seconds_from_now: 秒数（type=relative时必填）\n"
            "  message: 可选，闹钟响铃时播报的消息",
            PropertyList({
                Property("type", kPropertyTypeString),
                Property("hour", kPropertyTypeInteger),
                Property("minute", kPropertyTypeInteger),
                Property("seconds_from_now", kPropertyTypeInteger),
                Property("message", kPropertyTypeString),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                // 检查闹钟数量上限
                int enabled_count = 0;
                for (const auto& a : alarms_) {
                    if (a.enabled) enabled_count++;
                }
                if (enabled_count >= MAX_ALARMS) {
                    cJSON* err = cJSON_CreateObject();
                    cJSON_AddBoolToObject(err, "success", false);
                    cJSON_AddStringToObject(err, "error", "Too many alarms");
                    return err;
                }

                std::string type = properties["type"].value<std::string>();
                std::string message = properties["message"].value<std::string>();

                AlarmEntry alarm;
                alarm.id = GetNextAlarmId();
                alarm.enabled = true;
                alarm.message = message;

                time_t now = time(NULL);
                if (now < 2025UL * 365 * 24 * 3600) {
                    now = 0;  // 时间未同步
                }

                if (type == "relative") {
                    int seconds = properties["seconds_from_now"].value<int>();
                    if (seconds <= 0) {
                        cJSON* err = cJSON_CreateObject();
                        cJSON_AddBoolToObject(err, "success", false);
                        cJSON_AddStringToObject(err, "error", "seconds_from_now must be positive");
                        return err;
                    }
                    alarm.trigger_time = now + seconds;
                    ESP_LOGI(TAG, "Relative alarm #%d: %d seconds from now -> %lld",
                             alarm.id, seconds, (long long)alarm.trigger_time);

                } else {  // absolute
                    int hour = properties["hour"].value<int>();
                    int minute = properties["minute"].value<int>();
                    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
                        cJSON* err = cJSON_CreateObject();
                        cJSON_AddBoolToObject(err, "success", false);
                        cJSON_AddStringToObject(err, "error", "Invalid hour/minute");
                        return err;
                    }

                    struct tm tm_now = {};
                    localtime_r(&now, &tm_now);
                    tm_now.tm_hour = hour;
                    tm_now.tm_min = minute;
                    tm_now.tm_sec = 0;
                    alarm.trigger_time = mktime(&tm_now);

                    // 如果目标时间已过，推到明天
                    if (alarm.trigger_time <= now) {
                        alarm.trigger_time += 24 * 3600;
                    }
                    ESP_LOGI(TAG, "Absolute alarm #%d: %02d:%02d -> %lld (%s)",
                             alarm.id, hour, minute,
                             (long long)alarm.trigger_time,
                             alarm.trigger_time > now + 24 * 3600 ? "tomorrow" : "today");
                }

                alarms_.push_back(alarm);
                SaveAlarms();

                // 格式化返回信息
                struct tm* tm = localtime(&alarm.trigger_time);
                char time_buf[32];
                strftime(time_buf, sizeof(time_buf), "%H:%M", tm);

                cJSON* reply = cJSON_CreateObject();
                cJSON_AddBoolToObject(reply, "success", true);
                cJSON_AddNumberToObject(reply, "id", alarm.id);
                cJSON_AddStringToObject(reply, "trigger_time", time_buf);
                int seconds_remaining = (int)(alarm.trigger_time - now);
                cJSON_AddNumberToObject(reply, "seconds_remaining", seconds_remaining);
                if (!message.empty()) {
                    cJSON_AddStringToObject(reply, "message", message.c_str());
                }
                return reply;
            });

        // ----- self.alarm.cancel -----
        mcp_server.AddTool("self.alarm.cancel",
            "取消指定的闹钟",
            PropertyList({
                Property("id", kPropertyTypeInteger),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int id = properties["id"].value<int>();

                for (auto& a : alarms_) {
                    if (a.id == id) {
                        a.enabled = false;
                        SaveAlarms();
                        cJSON* reply = cJSON_CreateObject();
                        cJSON_AddBoolToObject(reply, "success", true);
                        cJSON_AddNumberToObject(reply, "id", id);
                        cJSON_AddStringToObject(reply, "status", "cancelled");
                        ESP_LOGI(TAG, "Alarm #%d cancelled", id);
                        return reply;
                    }
                }
                cJSON* err = cJSON_CreateObject();
                cJSON_AddBoolToObject(err, "success", false);
                cJSON_AddStringToObject(err, "error", "Alarm not found");
                return err;
            });

        // ----- self.alarm.list -----
        mcp_server.AddTool("self.alarm.list",
            "列出所有已设置的闹钟",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                time_t now = time(NULL);
                auto* root = cJSON_CreateObject();
                cJSON_AddBoolToObject(root, "success", true);
                auto* arr = cJSON_AddArrayToObject(root, "alarms");

                for (const auto& a : alarms_) {
                    if (!a.enabled) continue;
                    auto* obj = cJSON_CreateObject();
                    cJSON_AddNumberToObject(obj, "id", a.id);
                    char time_buf[32];
                    struct tm* tm = localtime(&a.trigger_time);
                    strftime(time_buf, sizeof(time_buf), "%H:%M", tm);
                    cJSON_AddStringToObject(obj, "trigger_time", time_buf);
                    int remaining = (int)(a.trigger_time - now);
                    cJSON_AddNumberToObject(obj, "seconds_remaining", remaining);
                    if (!a.message.empty()) {
                        cJSON_AddStringToObject(obj, "message", a.message.c_str());
                    }
                    cJSON_AddItemToArray(arr, obj);
                }
                cJSON_AddNumberToObject(root, "count", cJSON_GetArraySize(arr));
                return root;
            });

        // ----- self.alarm.get_status -----
        mcp_server.AddTool("self.alarm.get_status",
            "获取闹钟系统的当前状态（是否有闹钟在响、已设置的闹钟数量等）",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                time_t now = time(NULL);
                int enabled_count = 0;
                int upcoming_count = 0;  // 未来闹钟数
                for (const auto& a : alarms_) {
                    if (a.enabled) {
                        enabled_count++;
                        if (a.trigger_time > now) upcoming_count++;
                    }
                }

                auto* reply = cJSON_CreateObject();
                cJSON_AddBoolToObject(reply, "success", true);
                cJSON_AddBoolToObject(reply, "alarm_active", alarm_active_);
                cJSON_AddNumberToObject(reply, "active_alarm_id", active_alarm_id_);
                cJSON_AddNumberToObject(reply, "total_alarms", enabled_count);
                cJSON_AddNumberToObject(reply, "upcoming_alarms", upcoming_count);
                if (alarm_active_) {
                    cJSON_AddNumberToObject(reply, "repeat_count", alarm_repeat_count_);
                }
                return reply;
            });
    }

    // ========================================================================
    // 电源管理
    // ========================================================================
    void PowerOn() {
        power_manager_.PowerOn();
        led_controller_.UpdateLedStatus(power_manager_);
        ESP_LOGI(TAG, "Device powered on.");
    }

    void PowerOff() {
        SetMotorState(false);
        power_manager_.PowerOff();
        led_controller_.UpdateLedStatus(power_manager_);
        Application::GetInstance().SetDeviceState(DeviceState::kDeviceStateIdle);
        ESP_LOGI(TAG, "Device powered off.");
    }

public:
    FogSeekNanoAlarm() : boot_button_(BOOT_BUTTON_GPIO), ctrl_button_(CTRL_BUTTON_GPIO) {
        ESP_LOGI(TAG, "=== FogSeek Nano Alarm Initializing ===");

        InitializeI2c();
        InitializeTca6408a();
        InitializePowerManager();
        InitializeLedController();
        InitializeAudioAmplifier();
        InitializeDisplayManager();
        InitializeButtonCallbacks();

        // 加载持久化闹钟
        LoadAlarms();

        // 创建闹钟检查定时器（1秒周期）
        esp_timer_create_args_t check_timer_args = {};
        check_timer_args.callback = AlarmCheckTimerCallback;
        check_timer_args.arg = this;
        check_timer_args.name = "alarm_check";
        check_timer_args.dispatch_method = ESP_TIMER_TASK;
        esp_timer_create(&check_timer_args, &alarm_check_timer_);
        esp_timer_start_periodic(alarm_check_timer_, 1000000);  // 1秒

        // 创建闹钟重复响铃定时器
        esp_timer_create_args_t repeat_timer_args = {};
        repeat_timer_args.callback = AlarmRepeatTimerCallback;
        repeat_timer_args.arg = this;
        repeat_timer_args.name = "alarm_repeat";
        repeat_timer_args.dispatch_method = ESP_TIMER_TASK;
        esp_timer_create(&repeat_timer_args, &alarm_repeat_timer_);

        // 创建 LED 闪烁定时器
        esp_timer_create_args_t led_timer_args = {};
        led_timer_args.callback = AlarmLedTimerCallback;
        led_timer_args.arg = this;
        led_timer_args.name = "alarm_led";
        led_timer_args.dispatch_method = ESP_TIMER_TASK;
        esp_timer_create(&led_timer_args, &alarm_led_timer_);

        // 注册 MCP 工具
        InitializeTools();

        // 电源状态回调
        power_manager_.SetPowerStateCallback([this](FogSeekPowerManager::PowerState state) {
            led_controller_.UpdateLedStatus(power_manager_);
        });

        ESP_LOGI(TAG, "=== FogSeek Nano Alarm Ready ===");
    }

    ~FogSeekNanoAlarm() {
        if (alarm_check_timer_) {
            esp_timer_stop(alarm_check_timer_);
            esp_timer_delete(alarm_check_timer_);
        }
        if (alarm_repeat_timer_) {
            esp_timer_stop(alarm_repeat_timer_);
            esp_timer_delete(alarm_repeat_timer_);
        }
        if (alarm_led_timer_) {
            esp_timer_stop(alarm_led_timer_);
            esp_timer_delete(alarm_led_timer_);
        }
        if (tca6408a_handle_.initialized) {
            tca6408a_deinit(&tca6408a_handle_);
        }
        if (i2c_bus_) {
            i2c_del_master_bus(i2c_bus_);
        }
    }

    virtual Display* GetDisplay() override {
        return dual_display_;
    }

    virtual AudioCodec* GetAudioCodec() override {
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

    virtual Led* GetLed() override {
        return led_controller_.GetGreenLed();
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        auto power_state = power_manager_.GetPowerState();
        level = power_manager_.ReadBatteryLevel();
        charging = (power_state == FogSeekPowerManager::PowerState::USB_POWER_CHARGING);
        discharging = (power_state == FogSeekPowerManager::PowerState::BATTERY_POWER ||
                      power_state == FogSeekPowerManager::PowerState::LOW_BATTERY);
        return true;
    }
};

DECLARE_BOARD(FogSeekNanoAlarm);
