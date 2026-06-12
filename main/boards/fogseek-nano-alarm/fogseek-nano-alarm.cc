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
#include <algorithm>

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
// 闹钟重复模式
// ============================================================================
enum AlarmRepeatMode : uint8_t {
    REPEAT_ONCE     = 0,  // 单次
    REPEAT_DAILY    = 1,  // 每天
    REPEAT_WEEKDAYS = 2,  // 工作日（周一~周五）
    REPEAT_WEEKENDS = 3,  // 周末（周六~周日）
    REPEAT_WEEKLY   = 4,  // 每周特定星期几
    REPEAT_MONTHLY  = 5,  // 每月特定日
    REPEAT_YEARLY   = 6,  // 每年特定日期
};

// ============================================================================
// 闹钟条目数据结构
// ============================================================================
struct AlarmEntry {
    int id;                          // 闹钟ID（自增）
    time_t trigger_time;             // 下次触发的 Unix 时间戳
    int hour;                        // 蓝图：小时（0-23），repeat=ONCE 时为 -1
    int minute;                      // 蓝图：分钟（0-59），repeat=ONCE 时为 -1
    AlarmRepeatMode repeat;          // 重复模式
    int repeat_value;                // WEEKLY: wday(0=周日), MONTHLY: mday(1-31), YEARLY: month*100+day
    std::string label;               // 事件标签（如"开会"、"吃药"）
    std::string note;                // 额外备注
    bool enabled;
    bool skip_holidays;              // 是否跳过周末和节假日（仅对 daily/weekdays 生效）
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
    // 有效时间阈值：~2025-01-01 的 Unix epoch，用于判断 NTP 是否已同步
    static constexpr time_t MIN_VALID_TIME = (2025ULL - 1970) * 365 * 24 * 3600;

    // LED 闹钟闪烁
    esp_timer_handle_t alarm_led_timer_ = nullptr;
    bool alarm_led_state_ = false;

    // --- 节假日列表（存储当天0点的 epoch）---
    std::vector<time_t> holidays_;

    // ========================================================================
    // 闹钟持久化 (NVS -> JSON)
    // ========================================================================
    void SaveAlarms() {
        auto* root = cJSON_CreateArray();
        for (const auto& a : alarms_) {
            auto* obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(obj, "id", a.id);
            cJSON_AddNumberToObject(obj, "trigger_time", (double)a.trigger_time);
            cJSON_AddNumberToObject(obj, "hour", a.hour);
            cJSON_AddNumberToObject(obj, "minute", a.minute);
            cJSON_AddNumberToObject(obj, "repeat", (int)a.repeat);
            cJSON_AddNumberToObject(obj, "repeat_value", a.repeat_value);
            cJSON_AddStringToObject(obj, "label", a.label.c_str());
            cJSON_AddStringToObject(obj, "note", a.note.c_str());
            cJSON_AddStringToObject(obj, "message", a.label.c_str());  // 兼容旧版 message 字段
            cJSON_AddBoolToObject(obj, "enabled", a.enabled ? 1 : 0);
            if (a.skip_holidays) {
                cJSON_AddBoolToObject(obj, "skip_holidays", 1);
            }
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
        time_t now = time(NULL);
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, root) {
            AlarmEntry a;
            auto* id_n      = cJSON_GetObjectItem(item, "id");
            auto* trigger_n = cJSON_GetObjectItem(item, "trigger_time");
            auto* hour_n    = cJSON_GetObjectItem(item, "hour");
            auto* minute_n  = cJSON_GetObjectItem(item, "minute");
            auto* repeat_n  = cJSON_GetObjectItem(item, "repeat");
            auto* rv_n      = cJSON_GetObjectItem(item, "repeat_value");
            auto* label_n   = cJSON_GetObjectItem(item, "label");
            auto* note_n    = cJSON_GetObjectItem(item, "note");
            auto* msg_n     = cJSON_GetObjectItem(item, "message");  // 兼容旧版
            auto* en_n      = cJSON_GetObjectItem(item, "enabled");
            auto* skip_n    = cJSON_GetObjectItem(item, "skip_holidays");

            a.id           = id_n      ? id_n->valueint : 0;
            a.trigger_time = trigger_n ? (time_t)trigger_n->valuedouble : 0;
            a.hour         = hour_n    ? hour_n->valueint : -1;
            a.minute       = minute_n  ? minute_n->valueint : -1;
            a.repeat       = repeat_n  ? (AlarmRepeatMode)repeat_n->valueint : REPEAT_ONCE;
            a.repeat_value = rv_n      ? rv_n->valueint : -1;
            a.label        = label_n   ? label_n->valuestring : "";
            a.note         = note_n    ? note_n->valuestring : "";
            a.enabled      = en_n      ? (en_n->valueint != 0) : true;
            a.skip_holidays= skip_n    ? (skip_n->valueint != 0) : false;

            // 兼容旧版：如果只有 message 没有 label，用 message 作为 label
            if (a.label.empty() && msg_n && msg_n->valuestring) {
                a.label = msg_n->valuestring;
            }
            // 旧版没有 repeat 字段，hour 可能为 -1(相对时间) 或有效值
            if (a.hour < 0 && a.repeat == REPEAT_ONCE) {
                // 兼容旧版绝对时间闹钟：从 trigger_time 反推 hour/minute
                if (a.trigger_time > MIN_VALID_TIME) {
                    struct tm tm = {};
                    localtime_r(&a.trigger_time, &tm);
                    a.hour = tm.tm_hour;
                    a.minute = tm.tm_min;
                }
            }

            if (a.id >= next_alarm_id_) {
                next_alarm_id_ = a.id + 1;
            }

            // 重新计算重复闹钟的下次触发时间
            if (a.repeat != REPEAT_ONCE) {
                RecomputeNextTrigger(a, now);
                if (a.trigger_time < now - 60) {
                    ESP_LOGI(TAG, "Skipping expired repeat alarm #%d (label: %s)",
                             a.id, a.label.c_str());
                    continue;
                }
            } else if (now > MIN_VALID_TIME && a.trigger_time < now - 60) {
                // 单次闹钟已过期
                ESP_LOGI(TAG, "Pruning expired alarm #%d (label: %s)", a.id, a.label.c_str());
                continue;
            }
            alarms_.push_back(a);
            ESP_LOGI(TAG, "Loaded alarm #%d: %02d:%02d repeat=%d (label: %s)",
                     a.id, a.hour, a.minute, (int)a.repeat, a.label.c_str());
        }
        cJSON_Delete(root);
        ESP_LOGI(TAG, "Loaded %d alarms from NVS", (int)alarms_.size());
    }

    int GetNextAlarmId() {
        return next_alarm_id_++;
    }

    // ========================================================================
    // 节假日持久化
    // ========================================================================
    void SaveHolidays() {
        auto* root = cJSON_CreateArray();
        for (auto t : holidays_) {
            auto* obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(obj, "date", (double)t);
            cJSON_AddItemToArray(root, obj);
        }
        char* json_str = cJSON_PrintUnformatted(root);
        Settings nvs("alarms", true);
        nvs.SetString("holiday_list", json_str);
        cJSON_free(json_str);
        cJSON_Delete(root);
        ESP_LOGI(TAG, "Holidays saved (%d dates)", (int)holidays_.size());
    }

    void LoadHolidays() {
        Settings nvs("alarms", false);
        std::string json_str = nvs.GetString("holiday_list", "[]");
        auto* root = cJSON_Parse(json_str.c_str());
        if (!root || !cJSON_IsArray(root)) {
            if (root) cJSON_Delete(root);
            return;
        }
        holidays_.clear();
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, root) {
            auto* date_n = cJSON_GetObjectItem(item, "date");
            if (date_n) {
                holidays_.push_back((time_t)date_n->valuedouble);
            }
        }
        cJSON_Delete(root);
        // 清除过期节假日（保留未来1年内的）
        CleanExpiredHolidays();
        ESP_LOGI(TAG, "Loaded %d holidays", (int)holidays_.size());
    }

    // 清除已过期的节假日（7天前的自动清理）
    void CleanExpiredHolidays() {
        time_t now = time(NULL);
        time_t cutoff = now - 7 * 86400;
        auto it = holidays_.begin();
        while (it != holidays_.end()) {
            if (*it < cutoff) {
                it = holidays_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // 获取某天的 0 点 epoch（用于节假日比较）
    static time_t DateMidnight(time_t t) {
        struct tm tm = {};
        localtime_r(&t, &tm);
        tm.tm_hour = 0;
        tm.tm_min = 0;
        tm.tm_sec = 0;
        tm.tm_isdst = -1;
        return mktime(&tm);
    }

    // 判断某天是否为周末或节假日
    bool IsHolidayOrWeekend(time_t t) {
        struct tm tm = {};
        localtime_r(&t, &tm);
        // 周末：周六(6) 或 周日(0)
        if (tm.tm_wday == 0 || tm.tm_wday == 6) return true;
        // 检查是否在节假日列表中
        time_t midnight = DateMidnight(t);
        for (auto h : holidays_) {
            if (h == midnight) return true;
        }
        return false;
    }

    // ========================================================================
    // 重新计算闹钟的下次触发时间（重复闹钟用）
    // 核心原则：始终通过 localtime_r 获取完整 struct tm，修改字段后由 mktime 归一化
    // ========================================================================
    void RecomputeNextTrigger(AlarmEntry& alarm, time_t from_time) {
        if (alarm.repeat == REPEAT_ONCE) return;

        // 以 from_time 为基准，构建今日目标时刻的 struct tm
        struct tm tm = {};
        localtime_r(&from_time, &tm);
        tm.tm_hour = alarm.hour;
        tm.tm_min = alarm.minute;
        tm.tm_sec = 0;
        tm.tm_isdst = -1;
        time_t candidate = mktime(&tm);

        switch (alarm.repeat) {
            case REPEAT_DAILY: {
                // 每天：若已过则推到明天
                if (candidate <= from_time) candidate += 86400;
                break;
            }

            case REPEAT_WEEKDAYS: {
                // 工作日（周一~周五）
                if (candidate <= from_time) candidate += 86400;
                struct tm check = {};
                localtime_r(&candidate, &check);
                for (int i = 0; i < 7 && (check.tm_wday < 1 || check.tm_wday > 5); i++) {
                    candidate += 86400;
                    localtime_r(&candidate, &check);
                }
                break;
            }

            case REPEAT_WEEKENDS: {
                // 周末（周六、周日）
                if (candidate <= from_time) candidate += 86400;
                struct tm check = {};
                localtime_r(&candidate, &check);
                for (int i = 0; i < 7 && (check.tm_wday != 0 && check.tm_wday != 6); i++) {
                    candidate += 86400;
                    localtime_r(&candidate, &check);
                }
                break;
            }

            case REPEAT_WEEKLY: {
                // 每周特定星期几（repeat_value = 0~6, 0=周日）
                int target_wday = alarm.repeat_value;
                if (target_wday < 0 || target_wday > 6) target_wday = tm.tm_wday;
                struct tm check = {};
                localtime_r(&candidate, &check);
                int days_ahead = target_wday - check.tm_wday;
                if (days_ahead < 0) days_ahead += 7;
                candidate += days_ahead * 86400;
                if (candidate <= from_time) candidate += 7 * 86400;
                break;
            }

            case REPEAT_MONTHLY: {
                // 每月特定日（repeat_value = 1~31）
                int target_mday = alarm.repeat_value;
                if (target_mday < 1) target_mday = 1;
                if (target_mday > 28) target_mday = 28;  // 安全边界，所有月份都至少有28天

                struct tm m_tm = {};
                localtime_r(&from_time, &m_tm);
                m_tm.tm_hour = alarm.hour;
                m_tm.tm_min = alarm.minute;
                m_tm.tm_sec = 0;
                m_tm.tm_mday = target_mday;
                m_tm.tm_isdst = -1;
                candidate = mktime(&m_tm);
                if (candidate <= from_time) {
                    m_tm.tm_mon++;
                    if (m_tm.tm_mon > 11) { m_tm.tm_mon = 0; m_tm.tm_year++; }
                    candidate = mktime(&m_tm);
                }
                break;
            }

            case REPEAT_YEARLY: {
                // 每年特定日期（repeat_value = month*100 + day, 如 612 = 6月12日）
                int target_month = alarm.repeat_value / 100 - 1;  // 0-based
                int target_mday  = alarm.repeat_value % 100;
                if (target_month < 0 || target_month > 11) target_month = tm.tm_mon;
                if (target_mday < 1 || target_mday > 31)  target_mday  = tm.tm_mday;

                struct tm y_tm = {};
                localtime_r(&from_time, &y_tm);
                y_tm.tm_hour = alarm.hour;
                y_tm.tm_min = alarm.minute;
                y_tm.tm_sec = 0;
                y_tm.tm_mon = target_month;
                y_tm.tm_mday = target_mday;
                y_tm.tm_isdst = -1;
                candidate = mktime(&y_tm);
                if (candidate <= from_time) {
                    y_tm.tm_year++;
                    candidate = mktime(&y_tm);
                }
                break;
            }

            default: break;
        }

        // 如果启用了跳过节假日：推进到下一个非假日
        if (alarm.skip_holidays && (alarm.repeat == REPEAT_DAILY || alarm.repeat == REPEAT_WEEKDAYS)) {
            for (int safety = 0; safety < 366; safety++) {
                if (!IsHolidayOrWeekend(candidate)) break;
                candidate += 86400;
                // 重新对齐时/分（夏令时等边界情况）
                struct tm adj = {};
                localtime_r(&candidate, &adj);
                adj.tm_hour = alarm.hour;
                adj.tm_min = alarm.minute;
                adj.tm_sec = 0;
                adj.tm_isdst = -1;
                candidate = mktime(&adj);
            }
        }

        alarm.trigger_time = candidate;
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
        if (now < MIN_VALID_TIME) return;

        for (auto& alarm : alarms_) {
            if (!alarm.enabled) continue;
            if (now >= alarm.trigger_time) {
                // 节假日跳过：如果今天不应触发，直接重新排期
                if (alarm.skip_holidays && alarm.repeat != REPEAT_ONCE
                    && IsHolidayOrWeekend(now)) {
                    ESP_LOGI(TAG, "Alarm #%d (%s) skipped: today is holiday/weekend",
                             alarm.id, alarm.label.c_str());
                    RecomputeNextTrigger(alarm, now + 1);
                    SaveAlarms();
                    continue;
                }

                TriggerAlarm(alarm);

                if (alarm.repeat == REPEAT_ONCE) {
                    // 单次闹钟：触发后禁用
                    alarm.enabled = false;
                } else {
                    // 重复闹钟：重新计算下次触发时间
                    RecomputeNextTrigger(alarm, now + 1);  // +1 确保不重复触发
                    ESP_LOGI(TAG, "Alarm #%d rescheduled: next=%lu (repeat=%d)",
                             alarm.id, (unsigned long)alarm.trigger_time, (int)alarm.repeat);
                }
                SaveAlarms();
                break;  // 一次只触发一个闹钟
            }
        }
    }

    // ========================================================================
    // 触发闹钟
    // ========================================================================
    void TriggerAlarm(const AlarmEntry& alarm) {
        const char* label = alarm.label.empty() ? "提醒" : alarm.label.c_str();
        const char* repeat_str = "";
        switch (alarm.repeat) {
            case REPEAT_DAILY:    repeat_str = "每天"; break;
            case REPEAT_WEEKDAYS: repeat_str = "工作日"; break;
            case REPEAT_WEEKENDS: repeat_str = "周末"; break;
            case REPEAT_WEEKLY:   repeat_str = "每周"; break;
            case REPEAT_MONTHLY:  repeat_str = "每月"; break;
            case REPEAT_YEARLY:   repeat_str = "每年"; break;
            default: break;
        }

        ESP_LOGI(TAG, "================== ALARM FIRED ==================");
        ESP_LOGI(TAG, "Alarm #%d - label: \"%s\", repeat: %d%s",
                 alarm.id, label, (int)alarm.repeat,
                 *repeat_str ? " (recurring)" : "");

        alarm_active_ = true;
        active_alarm_id_ = alarm.id;
        alarm_repeat_count_ = 0;

        // 1. 振动电机
        StartMotorPulse();

        // 2. LED 红绿交替闪烁
        StartAlarmLedBlink();

        // 3. 播放闹钟音频
        Application::GetInstance().PlaySound(Lang::Sounds::OGG_EXCLAMATION);

        // 4. 启动重复响铃定时器
        esp_timer_start_periodic(alarm_repeat_timer_, ALARM_REPEAT_INTERVAL_MS * 1000);

        // 5. 表情显示为闹钟状态
        if (dual_display_) {
            dual_display_->SetEmotion("alarm");
        }

        // 6. 通过 AI 语音播报闹钟（短唤醒词 + MCP Notification 传递完整详情）
        auto& app = Application::GetInstance();
        std::string wake_msg = "闹钟-" + std::string(label);

        // 构建 MCP 详情（JSON）
        std::string mcp = "{";
        mcp += "\"jsonrpc\":\"2.0\",\"method\":\"notifications/alarm_triggered\",";
        mcp += "\"params\":{";
        mcp += "\"id\":" + std::to_string(alarm.id);
        mcp += ",\"label\":\"" + std::string(label) + "\"";
        if (!alarm.note.empty()) {
            mcp += ",\"note\":\"" + alarm.note + "\"";
        }
        if (alarm.repeat != REPEAT_ONCE) {
            mcp += ",\"repeat\":\"" + std::string(repeat_str) + "\"";
        }
        mcp += "}}";

        app.Schedule([wake_msg, mcp]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SendMcpMessage(mcp);
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
        mcp_server.AddTool("self.alarm.set",
            "【设置闹钟/提醒】创建一个新的闹钟或定时提醒。设备会在指定时刻振动电机、LED闪烁、播放音频通知用户。\n"
            "\n"
            "使用场景：\n"
            "1. 用户说「X点Y分提醒我...」→ 使用绝对时间（type='absolute', hour, minute）\n"
            "2. 用户说「N分钟后提醒我...」→ 使用相对时间（type='relative', seconds_from_now）\n"
            "3. 用户说「每天早上X点提醒我...」→ type='absolute' + repeat='daily'\n"
            "4. 用户说「每周一/三/五X点...」→ type='absolute' + repeat='weekly' + repeat_value=周几编码\n"
            "5. 用户说「每月N号X点...」→ type='absolute' + repeat='monthly' + repeat_value=N\n"
            "6. 用户说「开会」「吃药」等 → 设置label标签\n"
            "\n"
            "参数详解：\n"
            "  type: 时间类型。'absolute'=指定时刻(默认)，'relative'=从现在起N秒后\n"
            "  hour: 小时(0-23)，仅绝对时间需填写\n"
            "  minute: 分钟(0-59)，仅绝对时间需填写\n"
            "  seconds_from_now: 相对秒数(正整数)，仅相对时间需填写。例如：5分钟=300\n"
            "  repeat: 重复模式，可选值：\n"
            "    'once'(默认-单次), 'daily'(每天), 'weekdays'(周一至周五), 'weekends'(周六日),\n"
            "    'weekly'(每周指定星期), 'monthly'(每月指定日期), 'yearly'(每年指定日期)\n"
            "  repeat_value: 重复参数（仅 weekly/monthly/yearly 需填）：\n"
            "    weekly: 周几编码，0=周日,1=周一,2=周二,3=周三,4=周四,5=周五,6=周六\n"
            "    monthly: 几号(1-31，建议不超过28确保每月有效)\n"
            "    yearly: 月日编码，公式为 month*100+day。例如：6月12日=612, 12月3日=1203\n"
            "  label: 事件标签，如「开会」「吃药」「接孩子」。闹钟触发时设备会用此标签播报\n"
            "  note: 额外备注信息（可选）\n"
            "  message: 旧版兼容参数，等同于 label，新用户请直接使用 label\n"
            "  skip_holidays: 是否跳过周末和节假日（true/false，默认false）。仅对 daily/weekdays 重复模式生效。\n"
            "    当设为true时，闹钟在周六、周日及已设置的节假日不会触发，自动顺延到下一个工作日\n"
            "\n"
            "返回字段：success(是否成功), id(闹钟编号), trigger_time(触发时刻HH:MM),\n"
            "  seconds_remaining(剩余秒数), hour, minute, label, repeat, repeat_value\n"
            "\n"
            "重要提示：\n"
            "- 相对时间闹钟不会自动重复触发\n"
            "- 绝对时间+重复模式下，会从今天起计算下一个符合条件的时间点\n"
            "- 如果今天的指定时间已过，会自动推到明天或下一个符合条件的时间\n"
            "- 最多同时设置20个活跃闹钟\n"
            "- 相对时间闹钟在断电后会丢失",
            PropertyList({
                Property("type", kPropertyTypeString, std::string("absolute")),
                Property("hour", kPropertyTypeInteger, -1),
                Property("minute", kPropertyTypeInteger, -1),
                Property("seconds_from_now", kPropertyTypeInteger, -1),
                Property("repeat", kPropertyTypeString, std::string("once")),
                Property("repeat_value", kPropertyTypeInteger, -1),
                Property("label", kPropertyTypeString, std::string("")),
                Property("note", kPropertyTypeString, std::string("")),
                Property("message", kPropertyTypeString, std::string("")), // 兼容旧版
                Property("skip_holidays", kPropertyTypeBoolean, false),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
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

                std::string type  = properties["type"].value<std::string>();
                std::string rpt   = properties["repeat"].value<std::string>();
                int rv            = properties["repeat_value"].value<int>();
                std::string label = properties["label"].value<std::string>();
                std::string note  = properties["note"].value<std::string>();
                std::string message = properties["message"].value<std::string>();
                bool skip_hol = properties["skip_holidays"].value<bool>();

                // 兼容旧版：message 字段映射到 label
                if (label.empty() && !message.empty()) label = message;

                // 解析 repeat 模式
                AlarmRepeatMode repeat = REPEAT_ONCE;
                if (rpt == "daily")        repeat = REPEAT_DAILY;
                else if (rpt == "weekdays") repeat = REPEAT_WEEKDAYS;
                else if (rpt == "weekends") repeat = REPEAT_WEEKENDS;
                else if (rpt == "weekly")   repeat = REPEAT_WEEKLY;
                else if (rpt == "monthly")  repeat = REPEAT_MONTHLY;
                else if (rpt == "yearly")   repeat = REPEAT_YEARLY;

                // skip_holidays 仅对 daily/weekdays 生效
                if (skip_hol && repeat != REPEAT_DAILY && repeat != REPEAT_WEEKDAYS) {
                    skip_hol = false;
                }

                AlarmEntry alarm;
                alarm.id = GetNextAlarmId();
                alarm.enabled = true;
                alarm.label = label;
                alarm.note = note;
                alarm.repeat = repeat;
                alarm.repeat_value = rv;
                alarm.skip_holidays = skip_hol;

                time_t now = time(NULL);
                if (now < MIN_VALID_TIME) {
                    now = 0;
                }

                if (type == "relative") {
                    int seconds = properties["seconds_from_now"].value<int>();
                    if (seconds <= 0) {
                        cJSON* err = cJSON_CreateObject();
                        cJSON_AddBoolToObject(err, "success", false);
                        cJSON_AddStringToObject(err, "error", "请提供 seconds_from_now（大于0的秒数）");
                        return err;
                    }
                    // 相对时间：从当前时刻计算，存为 epoch
                    alarm.trigger_time = now + seconds;
                    alarm.hour = -1;
                    alarm.minute = -1;
                    if (repeat != REPEAT_ONCE) {
                        // 相对时间 + 重复模式：保存当前时刻的时/分作为蓝图
                        struct tm tm_now = {};
                        localtime_r(&alarm.trigger_time, &tm_now);
                        alarm.hour = tm_now.tm_hour;
                        alarm.minute = tm_now.tm_min;
                    }
                    ESP_LOGI(TAG, "Relative alarm #%d: %d sec -> %lu, repeat=%d (label: %s)",
                             alarm.id, seconds, (unsigned long)alarm.trigger_time,
                             (int)repeat, label.c_str());

                } else {  // absolute
                    int hour = properties["hour"].value<int>();
                    int minute = properties["minute"].value<int>();
                    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
                        cJSON* err = cJSON_CreateObject();
                        cJSON_AddBoolToObject(err, "success", false);
                        cJSON_AddStringToObject(err, "error", "请提供有效的 hour(0-23) 和 minute(0-59)");
                        return err;
                    }
                    alarm.hour = hour;
                    alarm.minute = minute;

                    // 用已验证的 localtime_r + mktime 模式计算 trigger_time
                    struct tm tm_now = {};
                    localtime_r(&now, &tm_now);
                    tm_now.tm_hour = hour;
                    tm_now.tm_min = minute;
                    tm_now.tm_sec = 0;
                    alarm.trigger_time = mktime(&tm_now);

                    // 重复闹钟用 RecomputeNextTrigger 推进到正确位置
                    if (repeat != REPEAT_ONCE) {
                        RecomputeNextTrigger(alarm, now);
                    } else if (alarm.trigger_time <= now) {
                        alarm.trigger_time += 86400;
                    }
                    ESP_LOGI(TAG, "Absolute alarm #%d: %02d:%02d repeat=%d (label: %s) -> next=%lu",
                             alarm.id, hour, minute, (int)repeat,
                             label.c_str(), (unsigned long)alarm.trigger_time);
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
                cJSON_AddNumberToObject(reply, "seconds_remaining", (int)(alarm.trigger_time - now));
                cJSON_AddNumberToObject(reply, "hour", alarm.hour);
                cJSON_AddNumberToObject(reply, "minute", alarm.minute);
                if (!label.empty()) {
                    cJSON_AddStringToObject(reply, "label", label.c_str());
                }
                if (repeat != REPEAT_ONCE) {
                    cJSON_AddStringToObject(reply, "repeat", rpt.c_str());
                    if (rv >= 0) cJSON_AddNumberToObject(reply, "repeat_value", rv);
                }
                if (alarm.skip_holidays) {
                    cJSON_AddBoolToObject(reply, "skip_holidays", true);
                }
                return reply;
            });

        // ----- self.alarm.cancel -----
        mcp_server.AddTool("self.alarm.cancel",
            "【取消闹钟】通过闹钟ID取消/删除一个已设置的闹钟。\n"
            "\n"
            "使用场景：\n"
            "1. 用户说「取消闹钟」但未指定ID → 先调用 self.alarm.list 列出所有闹钟，让用户确认是哪个\n"
            "2. 用户说「取消闹钟3号」→ 直接传入 id=3\n"
            "3. 用户说「取消那个提醒我吃药的闹钟」→ 先调用 self.alarm.list 查看label匹配的闹钟ID\n"
            "\n"
            "参数：\n"
            "  id: 闹钟编号（整数），可通过 self.alarm.list 获取\n"
            "\n"
            "返回：success(是否成功), id(被取消的闹钟编号), status(固定为'cancelled')\n"
            "失败时返回 success=false 及 error 说明",
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
            "【列出所有闹钟】查询当前设备上所有活跃（未取消、未过期）的闹钟列表。\n"
            "\n"
            "使用场景：\n"
            "1. 用户问「我设置了哪些闹钟？」→ 直接调用此工具\n"
            "2. 取消闹钟前确认ID → 先调用此工具获取闹钟列表和对应ID\n"
            "3. 用户问「距离最近的闹钟还有多久？」→ 调用此工具查看 seconds_remaining\n"
            "4. 了解某个label的闹钟设置 → 通过返回的 label 字段匹配\n"
            "\n"
            "无需参数，直接调用即可。\n"
            "\n"
            "返回字段（根级别）：success, count(活跃闹钟总数), alarms(闹钟数组)\n"
            "每个闹钟对象包含：id(编号), trigger_time(触发时刻HH:MM格式),\n"
            "  seconds_remaining(距离触发剩余秒数), label(事件标签，如有),\n"
            "  repeat(重复模式，如有), repeat_value(重复参数，如有)",
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
                    cJSON_AddNumberToObject(obj, "seconds_remaining", (int)(a.trigger_time - now));
                    if (!a.label.empty()) {
                        cJSON_AddStringToObject(obj, "label", a.label.c_str());
                    }
                    if (a.repeat != REPEAT_ONCE) {
                        const char* rs = "daily";
                        switch (a.repeat) {
                            case REPEAT_DAILY:    rs = "daily"; break;
                            case REPEAT_WEEKDAYS: rs = "weekdays"; break;
                            case REPEAT_WEEKENDS: rs = "weekends"; break;
                            case REPEAT_WEEKLY:   rs = "weekly"; break;
                            case REPEAT_MONTHLY:  rs = "monthly"; break;
                            case REPEAT_YEARLY:   rs = "yearly"; break;
                            default: break;
                        }
                        cJSON_AddStringToObject(obj, "repeat", rs);
                        if (a.repeat_value >= 0) {
                            cJSON_AddNumberToObject(obj, "repeat_value", a.repeat_value);
                        }
                        if (a.skip_holidays) {
                            cJSON_AddBoolToObject(obj, "skip_holidays", true);
                        }
                    }
                    cJSON_AddItemToArray(arr, obj);
                }
                cJSON_AddNumberToObject(root, "count", cJSON_GetArraySize(arr));
                return root;
            });

        // ----- self.alarm.get_status -----
        mcp_server.AddTool("self.alarm.get_status",
            "【闹钟系统状态】获取设备闹钟系统的实时运行状态概览。\n"
            "\n"
            "使用场景：\n"
            "1. 用户问「现在有没有闹钟在响？」→ 查看 alarm_active 字段\n"
            "2. 用户问「总共设了多少个闹钟？」→ 查看 total_alarms 字段\n"
            "3. 用户问「最近一次触发的是哪个闹钟？」→ 查看 active_alarm_id\n"
            "4. 想在设置新闹钟前了解当前闹钟数量 → 检查是否已达上限(20个)\n"
            "\n"
            "无需参数，直接调用即可。\n"
            "\n"
            "返回字段：success(是否成功), alarm_active(是否有闹钟正在响铃),\n"
            "  active_alarm_id(正在响铃的闹钟ID，无则为-1),\n"
            "  total_alarms(当前活跃闹钟总数), upcoming_alarms(尚未触发的未来闹钟数),\n"
            "  repeat_count(当前闹钟已重复响铃次数，仅响铃时有意义)",
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
                // 返回节假日数量
                cJSON_AddNumberToObject(reply, "holiday_count", (int)holidays_.size());
                return reply;
            });

        // ----- self.alarm.holiday.set -----
        mcp_server.AddTool("self.alarm.holiday.set",
            "【设置节假日】添加一个或多个日期到节假日列表。设置后，所有 skip_holidays=true 的闹钟在这些日期不会触发。\n"
            "\n"
            "使用场景：\n"
            "1. 用户说「元旦放假不要叫我」→ 调用此工具添加元旦日期\n"
            "2. 用户说「春节假期期间不用提醒我开会」→ 添加春节日期范围\n"
            "3. AI 自动维护：当用户设置 skip_holidays 闹钟时，AI 可主动查询当年节假日并同步到这\n"
            "\n"
            "参数：\n"
            "  dates: 日期数组，每个元素为字符串，格式 'YYYY-MM-DD'。例如 ['2026-06-15', '2026-06-16']\n"
            "  action: 'add'(默认-添加) 或 'clear_all'(清空所有节假日)\n"
            "\n"
            "返回：success, action, added_count(新增的节假日数量), total_count(当前节假日总数)",
            PropertyList({
                Property("dates", kPropertyTypeString, std::string("[]")),
                Property("action", kPropertyTypeString, std::string("add")),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string action = properties["action"].value<std::string>();

                if (action == "clear_all") {
                    int old_count = (int)holidays_.size();
                    holidays_.clear();
                    SaveHolidays();
                    auto* reply = cJSON_CreateObject();
                    cJSON_AddBoolToObject(reply, "success", true);
                    cJSON_AddStringToObject(reply, "action", "clear_all");
                    cJSON_AddNumberToObject(reply, "cleared_count", old_count);
                    cJSON_AddNumberToObject(reply, "total_count", 0);
                    return reply;
                }

                // action == "add"
                std::string dates_str = properties["dates"].value<std::string>();
                auto* dates_arr = cJSON_Parse(dates_str.c_str());
                if (!dates_arr || !cJSON_IsArray(dates_arr)) {
                    if (dates_arr) cJSON_Delete(dates_arr);
                    auto* err = cJSON_CreateObject();
                    cJSON_AddBoolToObject(err, "success", false);
                    cJSON_AddStringToObject(err, "error", "请提供 dates 数组，格式 ['YYYY-MM-DD', ...]");
                    return err;
                }

                int added = 0;
                cJSON* date_item = nullptr;
                cJSON_ArrayForEach(date_item, dates_arr) {
                    if (!cJSON_IsString(date_item)) continue;
                    std::string ds = date_item->valuestring;
                    // 解析 YYYY-MM-DD
                    int y = 0, m = 0, d = 0;
                    if (sscanf(ds.c_str(), "%d-%d-%d", &y, &m, &d) != 3) continue;
                    if (y < 2025 || m < 1 || m > 12 || d < 1 || d > 31) continue;

                    struct tm tm = {};
                    tm.tm_year = y - 1900;
                    tm.tm_mon = m - 1;
                    tm.tm_mday = d;
                    tm.tm_hour = 0;
                    tm.tm_min = 0;
                    tm.tm_sec = 0;
                    tm.tm_isdst = -1;
                    time_t midnight = mktime(&tm);

                    // 去重
                    bool exists = false;
                    for (auto h : holidays_) {
                        if (h == midnight) { exists = true; break; }
                    }
                    if (!exists) {
                        holidays_.push_back(midnight);
                        added++;
                    }
                }
                cJSON_Delete(dates_arr);

                SaveHolidays();

                // 对 skip_holidays 闹钟重新计算下次触发时间（因为新增了节假日）
                time_t now = time(NULL);
                for (auto& alarm : alarms_) {
                    if (alarm.skip_holidays && alarm.enabled && alarm.repeat != REPEAT_ONCE) {
                        RecomputeNextTrigger(alarm, now);
                    }
                }
                SaveAlarms();

                auto* reply = cJSON_CreateObject();
                cJSON_AddBoolToObject(reply, "success", true);
                cJSON_AddStringToObject(reply, "action", "add");
                cJSON_AddNumberToObject(reply, "added_count", added);
                cJSON_AddNumberToObject(reply, "total_count", (int)holidays_.size());
                return reply;
            });

        // ----- self.alarm.holiday.list -----
        mcp_server.AddTool("self.alarm.holiday.list",
            "【列出节假日】查询当前已设置的节假日列表。\n"
            "\n"
            "使用场景：\n"
            "1. 用户问「当前设了哪些节假日？」→ 直接调用\n"
            "2. AI 确认是否需要补充节假日 → 先列出已有节假日，再决定是否添加新的\n"
            "\n"
            "无需参数，直接调用即可。\n"
            "\n"
            "返回：success, count(节假日总数), holidays(日期数组，每个元素为 {date: 'YYYY-MM-DD', is_weekend: bool})",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                auto* root = cJSON_CreateObject();
                cJSON_AddBoolToObject(root, "success", true);
                auto* arr = cJSON_AddArrayToObject(root, "holidays");

                // 按时间排序
                std::vector<time_t> sorted = holidays_;
                std::sort(sorted.begin(), sorted.end());

                for (auto t : sorted) {
                    auto* obj = cJSON_CreateObject();
                    char date_buf[16];
                    struct tm tm = {};
                    localtime_r(&t, &tm);
                    strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &tm);
                    cJSON_AddStringToObject(obj, "date", date_buf);
                    cJSON_AddBoolToObject(obj, "is_weekend", tm.tm_wday == 0 || tm.tm_wday == 6);
                    cJSON_AddItemToArray(arr, obj);
                }
                cJSON_AddNumberToObject(root, "count", (int)sorted.size());
                return root;
            });

        // ----- self.alarm.holiday.remove -----
        mcp_server.AddTool("self.alarm.holiday.remove",
            "【移除节假日】从节假日列表中移除指定日期的节假日。\n"
            "\n"
            "使用场景：\n"
            "1. 用户说「某天不是节假日，帮我恢复那天的闹钟」→ 调用此工具移除\n"
            "\n"
            "参数：\n"
            "  dates: 日期数组，格式 ['YYYY-MM-DD', ...]，与 holiday.set 相同\n"
            "\n"
            "返回：success, removed_count, total_count",
            PropertyList({
                Property("dates", kPropertyTypeString, std::string("[]")),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string dates_str = properties["dates"].value<std::string>();
                auto* dates_arr = cJSON_Parse(dates_str.c_str());
                if (!dates_arr || !cJSON_IsArray(dates_arr)) {
                    if (dates_arr) cJSON_Delete(dates_arr);
                    auto* err = cJSON_CreateObject();
                    cJSON_AddBoolToObject(err, "success", false);
                    cJSON_AddStringToObject(err, "error", "请提供 dates 数组");
                    return err;
                }

                std::vector<time_t> to_remove;
                cJSON* date_item = nullptr;
                cJSON_ArrayForEach(date_item, dates_arr) {
                    if (!cJSON_IsString(date_item)) continue;
                    std::string ds = date_item->valuestring;
                    int y = 0, m = 0, d = 0;
                    if (sscanf(ds.c_str(), "%d-%d-%d", &y, &m, &d) != 3) continue;
                    struct tm tm = {};
                    tm.tm_year = y - 1900;
                    tm.tm_mon = m - 1;
                    tm.tm_mday = d;
                    tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
                    tm.tm_isdst = -1;
                    to_remove.push_back(mktime(&tm));
                }
                cJSON_Delete(dates_arr);

                int removed = 0;
                for (auto target : to_remove) {
                    auto it = std::find(holidays_.begin(), holidays_.end(), target);
                    if (it != holidays_.end()) {
                        holidays_.erase(it);
                        removed++;
                    }
                }

                SaveHolidays();

                auto* reply = cJSON_CreateObject();
                cJSON_AddBoolToObject(reply, "success", true);
                cJSON_AddNumberToObject(reply, "removed_count", removed);
                cJSON_AddNumberToObject(reply, "total_count", (int)holidays_.size());
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
        // 加载节假日列表
        LoadHolidays();

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
