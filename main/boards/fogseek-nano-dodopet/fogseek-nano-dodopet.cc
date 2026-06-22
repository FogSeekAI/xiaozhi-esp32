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
#include "uart_transport.h"
#include "wifi_manager.h"
#include <ssid_manager.h>
#include <esp_wifi.h>
#include <cstring>

#define TAG "FogSeekNanoDodopetDodopet"

// WiFi配网相关常量定义
#define WIFI_CMD_CONNECT       0x01  // 设置并连接WiFi
#define WIFI_CMD_CLEAR_CONFIG  0x02  // 清除已保存WiFi配置
#define WIFI_CMD_QUERY_STATUS  0x03  // 查询WiFi状态

// WiFi状态码
#define WIFI_STATUS_IDLE         0x00  // 空闲/未配网
#define WIFI_STATUS_RECEIVED     0x01  // 已收到配网请求
#define WIFI_STATUS_CONNECTING   0x02  // 连接中
#define WIFI_STATUS_CONNECTED    0x03  // 已连接
#define WIFI_STATUS_FAILED       0x04  // 连接失败
#define WIFI_STATUS_TIMEOUT      0x05  // 连接超时
#define WIFI_STATUS_CLEARED      0x06  // 已清除配置

// WiFi原因码
#define WIFI_REASON_NONE            0x00  // 无错误
#define WIFI_REASON_SSID_EMPTY      0x01  // SSID为空
#define WIFI_REASON_PASSWORD_FORMAT 0x02  // 密码格式错误
#define WIFI_REASON_SSID_NOT_FOUND  0x03  // 未扫描到目标SSID
#define WIFI_REASON_AUTH_FAILED     0x04  // 认证失败
#define WIFI_REASON_DHCP_FAILED     0x05  // DHCP失败
#define WIFI_REASON_TIMEOUT         0x06  // 连接超时
#define WIFI_REASON_DRIVER_FAILED   0x07  // WiFi驱动/SDK返回失败


// 动物声音资源列表
static const std::string_view CAT_VOICE_SOUNDS[] = {
    Lang::Sounds::OGG_CAT_VOICE01,
    
};

static const std::string_view DOG_VOICE_SOUNDS[] = {
    
    Lang::Sounds::OGG_DOG_VOICE03,
    
};

class FogSeekNanoDodopet : public WifiBoard
{
private:
    Button boot_button_;
    Button ctrl_button_;
    FogSeekPowerManager power_manager_;
    FogSeekLedController led_controller_;
    UartTransport uart_transport_;

    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    AudioCodec *audio_codec_ = nullptr;
    esp_timer_handle_t check_idle_timer_ = nullptr;

    uint8_t cat_sound_index_ = 0;
    uint8_t dog_sound_index_ = 0;

    // WiFi配网相关状态
    bool wifi_provisioning_in_progress_ = false;
    esp_timer_handle_t wifi_status_report_timer_ = nullptr;


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

    // 初始化 UART串口
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

        case MSG_TYPE_WIFI_PROVISIONING:
            success = HandleWifiProvisioning(content);
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

    // 解析WiFi配网请求帧
    bool ParseWifiProvisioningFrame(const std::string &content, 
                                   uint8_t &command, 
                                   uint8_t &flags,
                                   std::string &ssid,
                                   std::string &password)
    {
        if (content.size() < 4)
        {
            ESP_LOGE(TAG, "WiFi provisioning frame too short: %zu bytes", content.size());
            return false;
        }

        size_t offset = 0;
        
        // 读取命令（1字节）
        command = static_cast<uint8_t>(content[offset++]);
        
        // 读取标志位（1字节）
        flags = static_cast<uint8_t>(content[offset++]);
        
        // 读取SSID长度（1字节）
        uint8_t ssid_len = static_cast<uint8_t>(content[offset++]);
        
        // 读取密码长度（1字节）
        uint8_t password_len = static_cast<uint8_t>(content[offset++]);
        
        // 校验剩余长度
        if (content.size() < offset + ssid_len + password_len)
        {
            ESP_LOGE(TAG, "WiFi provisioning frame length mismatch");
            return false;
        }
        
        // 读取SSID
        if (ssid_len > 0)
        {
            ssid.assign(content.data() + offset, ssid_len);
            offset += ssid_len;
        }
        
        // 读取密码
        if (password_len > 0)
        {
            password.assign(content.data() + offset, password_len);
            offset += password_len;
        }
        
        ESP_LOGI(TAG, "Parsed WiFi provisioning: cmd=%d, flags=0x%02X, ssid_len=%d, pwd_len=%d",
                command, flags, ssid_len, password_len);
        
        return true;
    }

    // 发送WiFi状态帧
    void SendWifiStatusFrame(uint8_t status_code, uint8_t reason_code, int8_t rssi, const std::string &ip_address)
    {
        std::string payload;
        
        // 状态码（1字节）
        payload.push_back(static_cast<char>(status_code));
        
        // 原因码（1字节）
        payload.push_back(static_cast<char>(reason_code));
        
        // RSSI（1字节，有符号）
        payload.push_back(static_cast<char>(rssi));
        
        // IP地址长度（1字节）
        uint8_t ip_len = static_cast<uint8_t>(ip_address.size());
        payload.push_back(static_cast<char>(ip_len));
        
        // IP地址（N字节）
        if (ip_len > 0)
        {
            payload.append(ip_address);
        }
        
        uart_transport_.SendRawFrame(MSG_TYPE_WIFI_STATUS, payload);
        ESP_LOGI(TAG, "Sent WiFi status: status=%d, reason=%d, rssi=%d, ip=%s",
                status_code, reason_code, rssi, ip_address.c_str());
    }

    // 获取当前WiFi状态
    void GetCurrentWifiStatus(uint8_t &status_code, uint8_t &reason_code, int8_t &rssi, std::string &ip_address)
    {
        auto &wifi_manager = WifiManager::GetInstance();
        
        if (!wifi_manager.IsInitialized())
        {
            status_code = WIFI_STATUS_IDLE;
            reason_code = WIFI_REASON_NONE;
            rssi = 0;
            ip_address = "";
            return;
        }
        
        if (wifi_manager.IsConnected())
        {
            status_code = WIFI_STATUS_CONNECTED;
            reason_code = WIFI_REASON_NONE;
            
            // 获取RSSI
            wifi_ap_record_t ap_info;
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
            {
                rssi = ap_info.rssi;
            }
            else
            {
                rssi = 0;
            }
            
            // 获取IP地址
            esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (netif)
            {
                esp_netif_ip_info_t ip_info;
                if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK)
                {
                    char ip_str[16];
                    sprintf(ip_str, "%d.%d.%d.%d",
                            IP2STR(&ip_info.ip));
                    ip_address = ip_str;
                }
            }
        }
        else
        {
            status_code = WIFI_STATUS_IDLE;
            reason_code = WIFI_REASON_NONE;
            rssi = 0;
            ip_address = "";
        }
    }

    // 处理WiFi配网请求
    bool HandleWifiProvisioning(const std::string &content)
    {
        uint8_t command, flags;
        std::string ssid, password;
        
        // 解析配网请求帧
        if (!ParseWifiProvisioningFrame(content, command, flags, ssid, password))
        {
            ESP_LOGE(TAG, "Failed to parse WiFi provisioning frame");
            uart_transport_.SendErrorResponse(ERROR_WIFI_INVALID_CMD);
            return false;
        }
        
        switch (command)
        {
        case WIFI_CMD_CONNECT:
            return HandleWifiConnect(command, flags, ssid, password);
            
        case WIFI_CMD_CLEAR_CONFIG:
            return HandleWifiClearConfig();
            
        case WIFI_CMD_QUERY_STATUS:
            return HandleWifiQueryStatus();
            
        default:
            ESP_LOGW(TAG, "Unknown WiFi command: 0x%02X", command);
            uart_transport_.SendErrorResponse(ERROR_WIFI_INVALID_CMD);
            return false;
        }
    }

    // 处理WiFi连接命令
    bool HandleWifiConnect(uint8_t command, uint8_t flags, const std::string &ssid, const std::string &password)
    {
        ESP_LOGI(TAG, "Handling WiFi connect command");
        
        // 校验SSID长度（1-32字节）
        if (ssid.empty() || ssid.size() > 32)
        {
            ESP_LOGE(TAG, "Invalid SSID length: %zu", ssid.size());
            uart_transport_.SendErrorResponse(ERROR_WIFI_SSID_LEN);
            return false;
        }
        
        // 校验密码长度（0-64字节）
        if (password.size() > 64)
        {
            ESP_LOGE(TAG, "Invalid password length: %zu", password.size());
            uart_transport_.SendErrorResponse(ERROR_WIFI_PASSWORD_LEN);
            return false;
        }
        
        // 检查设备是否繁忙
        if (wifi_provisioning_in_progress_)
        {
            ESP_LOGW(TAG, "Device is busy with previous provisioning");
            uart_transport_.SendErrorResponse(ERROR_DEVICE_BUSY);
            return false;
        }
        
        // 标记配网进行中
        wifi_provisioning_in_progress_ = true;
        
        // 立即返回ACK成功
        ESP_LOGI(TAG, "WiFi provisioning started for SSID: %s", ssid.c_str());
        
        // 上报"已收到配网请求"状态
        SendWifiStatusFrame(WIFI_STATUS_RECEIVED, WIFI_REASON_NONE, 0, "");
        
        // 在后台任务中执行WiFi连接
        xTaskCreate([](void *arg)
                    {
            auto instance = static_cast<FogSeekNanoDodopet*>(arg);
            instance->ExecuteWifiConnect(instance->ssid_to_connect_, 
                                        instance->password_to_connect_,
                                        instance->flags_to_connect_);
            vTaskDelete(nullptr); },
                    "wifi_connect_task", 4096, this, 5, nullptr);
        
        // 保存参数供后台任务使用
        ssid_to_connect_ = ssid;
        password_to_connect_ = password;
        flags_to_connect_ = flags;
        
        return true;
    }

    // 执行WiFi连接（在后台任务中运行）
    void ExecuteWifiConnect(const std::string &ssid, const std::string &password, uint8_t flags)
    {
        ESP_LOGI(TAG, "Executing WiFi connect with full channel scan...");
        
        auto &wifi_manager = WifiManager::GetInstance();
        auto &ssid_manager = SsidManager::GetInstance();
        
        // 1. 保存凭证
        ssid_manager.AddSsid(ssid, password);
        
        // 上报"连接中"状态
        SendWifiStatusFrame(WIFI_STATUS_CONNECTING, WIFI_REASON_NONE, 0, "");
        
        // 2. 彻底重置 WiFi 状态
        ESP_LOGI(TAG, "Stopping all WiFi modes...");
        wifi_manager.StopStation();
        if (wifi_manager.IsConfigMode()) {
            wifi_manager.StopConfigAp();
        }
        vTaskDelay(pdMS_TO_TICKS(500)); // 等待射频彻底空闲
        
        // 3. 重新初始化 Station
        if (!wifi_manager.IsInitialized()) {
            WifiManagerConfig config;
            config.ssid_prefix = "Dodopet";
            config.language = Lang::CODE;
            wifi_manager.Initialize(config);
        }
        
        ESP_LOGI(TAG, "Starting Station and performing full scan...");
        wifi_manager.StartStation();
        
        // 【核心修复】执行全信道主动扫描，确保能跨越信道找到目标
        wifi_scan_config_t scan_config = {};
        scan_config.ssid = nullptr; // 扫描所有 SSID
        scan_config.bssid = nullptr;
        scan_config.channel = 0;    // 扫描所有信道 (1-13)
        scan_config.show_hidden = true;
        scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
        
        // 增加每个信道的扫描时间，提高捕获率
        scan_config.scan_time.active.min = 120;
        scan_config.scan_time.active.max = 360;

        esp_err_t err = esp_wifi_scan_start(&scan_config, true);
        if (err == ESP_OK) {
            uint16_t ap_count = 0;
            esp_wifi_scan_get_ap_num(&ap_count);
            ESP_LOGI(TAG, "Full scan completed. Found %d APs.", ap_count);
            
            // 检查目标是否在列表中
            if (ap_count > 0) {
                wifi_ap_record_t* ap_list = new wifi_ap_record_t[ap_count];
                esp_wifi_scan_get_ap_records(&ap_count, ap_list);
                
                bool found = false;
                for (int i = 0; i < ap_count; i++) {
                    std::string found_ssid((char*)ap_list[i].ssid);
                    ESP_LOGI(TAG, "  [%d] SSID: '%s', RSSI: %d, Channel: %d, Auth: %d", 
                             i, found_ssid.c_str(), ap_list[i].rssi, ap_list[i].primary, ap_list[i].authmode);
                    
                    if (found_ssid == ssid) {
                        found = true;
                        ESP_LOGI(TAG, "  >>> FOUND TARGET: %s <<<", ssid.c_str());
                    }
                }
                
                if (!found) {
                    ESP_LOGW(TAG, "Target AP '%s' NOT found in this scan!", ssid.c_str());
                }
                
                delete[] ap_list;
            } else {
                ESP_LOGW(TAG, "No APs found during full scan.");
            }
        } else {
            ESP_LOGE(TAG, "Scan failed with error: %s", esp_err_to_name(err));
        }

        // 4. 等待连接（延长超时以应对信道切换）
        bool connected = false;
        int timeout_ms = 40000; 
        int elapsed_ms = 0;
        
        while (elapsed_ms < timeout_ms)
        {
            vTaskDelay(pdMS_TO_TICKS(200));
            elapsed_ms += 200;
            
            if (wifi_manager.IsConnected())
            {
                connected = true;
                break;
            }
        }
        
        if (connected)
        {
            ESP_LOGI(TAG, "WiFi connected successfully");
            uint8_t status_code, reason_code;
            int8_t rssi;
            std::string ip_address;
            GetCurrentWifiStatus(status_code, reason_code, rssi, ip_address);
            SendWifiStatusFrame(status_code, reason_code, rssi, ip_address);
        }
        else
        {
            ESP_LOGE(TAG, "WiFi connection timed out");
            SendWifiStatusFrame(WIFI_STATUS_TIMEOUT, WIFI_REASON_TIMEOUT, 0, "");
        }
        
        wifi_provisioning_in_progress_ = false;
    }

    // 处理清除WiFi配置命令
    bool HandleWifiClearConfig()
    {
        ESP_LOGI(TAG, "Handling WiFi clear config command");
        
        auto &wifi_manager = WifiManager::GetInstance();
        auto &ssid_manager = SsidManager::GetInstance();
        
        // 清除所有保存的WiFi配置（通过循环删除）
        while (!ssid_manager.GetSsidList().empty())
        {
            ssid_manager.RemoveSsid(0);
        }
        
        // 断开当前连接
        if (wifi_manager.IsInitialized())
        {
            wifi_manager.StopStation();
        }
        
        // 上报"已清除配置"状态
        SendWifiStatusFrame(WIFI_STATUS_CLEARED, WIFI_REASON_NONE, 0, "");
        
        ESP_LOGI(TAG, "WiFi configuration cleared");
        return true;
    }

    // 处理查询WiFi状态命令
    bool HandleWifiQueryStatus()
    {
        ESP_LOGI(TAG, "Handling WiFi query status command");
        
        uint8_t status_code, reason_code;
        int8_t rssi;
        std::string ip_address;
        
        GetCurrentWifiStatus(status_code, reason_code, rssi, ip_address);
        
        // 上报当前状态
        SendWifiStatusFrame(status_code, reason_code, rssi, ip_address);
        
        return true;
    }

    // 开机流程
    void PowerOn()
    {
        power_manager_.PowerOn();                        // 更新电源状态
        led_controller_.UpdateLedStatus(power_manager_); // 更新LED灯状态
        ESP_LOGI(TAG, "Device powered on.");
        HandleAutoWake(); // 开机自动唤醒
    }

    // 关机流程
    void PowerOff()
    {
        power_manager_.PowerOff();
        led_controller_.UpdateLedStatus(power_manager_);
        Application::GetInstance().SetDeviceState(DeviceState::kDeviceStateIdle); // 关机后将设备状态设置为空闲，便于下次开机自动唤醒
        ESP_LOGI(TAG, "Device powered off.");
    }

public:
    FogSeekNanoDodopet() : boot_button_(BOOT_BUTTON_GPIO), ctrl_button_(CTRL_BUTTON_GPIO)
    {
        InitializeI2c();
        InitializePowerManager();
        InitializeLedController();
        InitializeButtonCallbacks();
        InitializeUart();

        // 设置电源状态变化回调函数，充电时，充电状态变化更新指示灯
        power_manager_.SetPowerStateCallback([this](FogSeekPowerManager::PowerState state)
                                             { led_controller_.UpdateLedStatus(power_manager_); });

        // 初始化WiFi配网相关变量
        wifi_provisioning_in_progress_ = false;
        wifi_status_report_timer_ = nullptr;
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
        wifi_manager.SetEventCallback([this](WifiEvent event, const std::string& data)
                                      {
            switch (event) {
                case WifiEvent::Scanning:
                    OnNetworkEvent(NetworkEvent::Scanning);
                    break;
                case WifiEvent::Connecting:
                    OnNetworkEvent(NetworkEvent::Connecting, data);
                    break;
                case WifiEvent::Connected:
                    OnNetworkEvent(NetworkEvent::Connected, data);
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

        if (wifi_status_report_timer_)
        {
            esp_timer_stop(wifi_status_report_timer_);
            esp_timer_delete(wifi_status_report_timer_);
        }
    }

private:
    // WiFi配网临时存储
    std::string ssid_to_connect_;
    std::string password_to_connect_;
    uint8_t flags_to_connect_;
};

DECLARE_BOARD(FogSeekNanoDodopet);