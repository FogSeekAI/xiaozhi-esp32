#include "mcp_tools.h"
#include "led_controller.h"
#include "motor_controller.h"
#include "fragrance_controller.h"
#include "power_manager.h"
#include "mcp_server.h"
#include "application.h"
#include "board.h"
#include "display.h"
#include "esp_sleep.h"
#include <esp_log.h>
#include <cJSON.h>
#include <sstream>
#include <vector>
#include <tuple>

static const char *TAG = "McpTools";

void InitializeSystemMCP(
    McpServer &mcp_server,
    FogSeekPowerManager &power_manager)
{
    // 添加系统关机的工具函数
    mcp_server.AddUserOnlyTool("self.shutdown",
                               "Shutdown the system. Command words examples: 关机, 关闭设备, 设备关机, 关闭系统",
                               PropertyList(),
                               [&power_manager](const PropertyList &properties) -> ReturnValue
                               {
                                   auto &app = Application::GetInstance();
                                   auto &board = Board::GetInstance();

                                   app.Schedule([&app, &board, &power_manager]()
                                                {
                                                    ESP_LOGW(TAG, "User requested shutdown");

                                                    // 显示关机通知
                                                    auto display = board.GetDisplay();
                                                    if (display)
                                                    {
                                                        display->ShowNotification("正在关机...", 3000);
                                                    }

                                                    power_manager.PowerOff(); });
                                   return true;
                               });
}

void InitializeDualColorLightMCP(
    McpServer &mcp_server,
    FogSeekLedController &led_controller)
{
    // 添加获取当前灯状态的工具函数
    mcp_server.AddTool("self.light.get_status",
                       "获取当前冷暖灯光的状态",
                       PropertyList(),
                       [&led_controller](const PropertyList &properties) -> ReturnValue
                       {
                           bool cold_light_state = led_controller.IsColdLightOn();
                           bool warm_light_state = led_controller.IsWarmLightOn();

                           // 使用字符串拼接方式返回JSON - 项目中最标准的做法
                           std::string status = "{\"cold_light\":" + std::string(cold_light_state ? "true" : "false") +
                                                ",\"warm_light\":" + std::string(warm_light_state ? "true" : "false") + "}";
                           return status;
                       });

    // 添加设置冷暖灯光亮度的工具函数
    mcp_server.AddTool("self.light.set_brightness",
                       "设置冷暖灯光的亮度，冷光和暖光可以独立调节，亮度范围为0-100，关灯为0，开灯默认为30亮度。"
                       "根据用户情绪描述调节冷暖灯光亮度，大模型应该分析用户的话语，理解用户的情绪状态和场景描述，然后根据情绪设置合适的冷暖灯光亮度组合。",
                       PropertyList({Property("cold_brightness", kPropertyTypeInteger, 0, 100),
                                     Property("warm_brightness", kPropertyTypeInteger, 0, 100)}),
                       [&led_controller](const PropertyList &properties) -> ReturnValue
                       {
                           int cold_brightness = properties["cold_brightness"].value<int>();
                           int warm_brightness = properties["warm_brightness"].value<int>();

                           // 设置冷暖灯光亮度
                           led_controller.SetColdLightBrightness(cold_brightness);
                           led_controller.SetWarmLightBrightness(warm_brightness);

                           // 根据亮度值决定是否开启灯光
                           led_controller.SetColdLight(cold_brightness > 0);
                           led_controller.SetWarmLight(warm_brightness > 0);

                           ESP_LOGI(TAG, "Color temperature set - Cold: %d%%, Warm: %d%%",
                                    cold_brightness, warm_brightness);

                           // 使用字符串拼接方式返回JSON - 项目中最标准的做法
                           std::string result = "{\"success\":true"
                                                ",\"cold_brightness\":" +
                                                std::to_string(cold_brightness) +
                                                ",\"warm_brightness\":" + std::to_string(warm_brightness) + "}";
                           return result;
                       });

    // 添加控制冷光灯开关的工具函数
    mcp_server.AddTool("self.light.set_cold_light",
                       "控制冷光灯的开关状态，适用于需要调整冷暖光平衡的场景。",
                       PropertyList({Property("state", kPropertyTypeBoolean, false)}),
                       [&led_controller](const PropertyList &properties) -> ReturnValue
                       {
                           bool state = properties["state"].value<bool>();

                           led_controller.SetColdLight(state);

                           ESP_LOGI(TAG, "Cold light control - State: %s", state ? "ON" : "OFF");

                           std::string response = "{\"success\":true"
                                                  ",\"state\":" +
                                                  std::string(state ? "true" : "false") + "}";
                           return response;
                       });

    // 添加控制暖光灯开关的工具函数
    mcp_server.AddTool("self.light.set_warm_light",
                       "控制暖光灯的开关状态，适用于需要调整冷暖光平衡的场景。",
                       PropertyList({Property("state", kPropertyTypeBoolean, false)}),
                       [&led_controller](const PropertyList &properties) -> ReturnValue
                       {
                           bool state = properties["state"].value<bool>();

                           led_controller.SetWarmLight(state);

                           ESP_LOGI(TAG, "Warm light control - State: %s", state ? "ON" : "OFF");

                           std::string response = "{\"success\":true"
                                                  ",\"state\":" +
                                                  std::string(state ? "true" : "false") + "}";
                           return response;
                       });
}

void InitializeRgbLedMCP(
    McpServer &mcp_server,
    FogSeekLedController &led_controller)
{
    // 添加RGB LED 统一设置所有灯光颜色的工具函数
    mcp_server.AddTool("self.light.set_all_color",
                       "统一设置所有灯光颜色，一次性更新所有 LED 灯珠颜色，可以控制任意颜色，适用于需要整体氛围一致的场景。",
                       PropertyList({Property("red", kPropertyTypeInteger, 0, 255),
                                     Property("green", kPropertyTypeInteger, 0, 255),
                                     Property("blue", kPropertyTypeInteger, 0, 255)}),
                       [&led_controller](const PropertyList &properties) -> ReturnValue
                       {
                           int red = properties["red"].value<int>();
                           int green = properties["green"].value<int>();
                           int blue = properties["blue"].value<int>();

                           StripColor color = {static_cast<uint8_t>(red),
                                               static_cast<uint8_t>(green),
                                               static_cast<uint8_t>(blue)};

                           led_controller.GetRgbLedStrip()->SetAllColor(color);
                           ESP_LOGI(TAG, "RGB LED all lights set to color - R: %d, G: %d, B: %d",
                                    red, green, blue);

                           std::string response = "{\"success\":true"
                                                  ",\"red\":" +
                                                  std::to_string(red) +
                                                  ",\"green\":" + std::to_string(green) +
                                                  ",\"blue\":" + std::to_string(blue) + "}";
                           return response;
                       });

    // 添加RGB LED 单个灯光颜色控制的工具函数
    mcp_server.AddTool("self.light.set_single_color",
                       "设置单个 LED 灯珠的颜色，可以指定灯珠索引和颜色值，通过重复调用可以依次设置每一个灯光成不同颜色，实现彩虹、渐变等复杂效果。",
                       PropertyList({Property("index", kPropertyTypeInteger, 0, 7),
                                     Property("red", kPropertyTypeInteger, 0, 255),
                                     Property("green", kPropertyTypeInteger, 0, 255),
                                     Property("blue", kPropertyTypeInteger, 0, 255)}),
                       [&led_controller](const PropertyList &properties) -> ReturnValue
                       {
                           int index = properties["index"].value<int>();
                           int red = properties["red"].value<int>();
                           int green = properties["green"].value<int>();
                           int blue = properties["blue"].value<int>();

                           StripColor color = {static_cast<uint8_t>(red),
                                               static_cast<uint8_t>(green),
                                               static_cast<uint8_t>(blue)};

                           led_controller.GetRgbLedStrip()->SetSingleColor(static_cast<uint8_t>(index), color);
                           ESP_LOGI(TAG, "RGB LED single light set - Index: %d, R: %d, G: %d, B: %d",
                                    index, red, green, blue);

                           std::string response = "{\"success\":true"
                                                  ",\"index\":" +
                                                  std::to_string(index) +
                                                  ",\"red\":" + std::to_string(red) +
                                                  ",\"green\":" + std::to_string(green) +
                                                  ",\"blue\":" + std::to_string(blue) + "}";
                           return response;
                       });

    // 添加RGB LED 闪烁效果的工具函数
    mcp_server.AddTool("self.light.blink",
                       "让所有 LED 灯珠以指定颜色和间隔时间进行闪烁，适用于警示、提醒等场景。",
                       PropertyList({Property("red", kPropertyTypeInteger, 0, 255),
                                     Property("green", kPropertyTypeInteger, 0, 255),
                                     Property("blue", kPropertyTypeInteger, 0, 255),
                                     Property("interval_ms", kPropertyTypeInteger, 500, 100, 5000)}),
                       [&led_controller](const PropertyList &properties) -> ReturnValue
                       {
                           int red = properties["red"].value<int>();
                           int green = properties["green"].value<int>();
                           int blue = properties["blue"].value<int>();
                           int interval_ms = properties["interval_ms"].value<int>();

                           StripColor color = {static_cast<uint8_t>(red),
                                               static_cast<uint8_t>(green),
                                               static_cast<uint8_t>(blue)};

                           led_controller.GetRgbLedStrip()->Blink(color, interval_ms);
                           ESP_LOGI(TAG, "RGB LED blink started - Color: R:%d, G:%d, B:%d, Interval: %dms",
                                    red, green, blue, interval_ms);

                           std::string response = "{\"success\":true"
                                                  ",\"red\":" +
                                                  std::to_string(red) +
                                                  ",\"green\":" + std::to_string(green) +
                                                  ",\"blue\":" + std::to_string(blue) +
                                                  ",\"interval_ms\":" + std::to_string(interval_ms) + "}";
                           return response;
                       });

    // 添加RGB LED 滚动效果的工具函数
    mcp_server.AddTool("self.light.scroll",
                       "创建 LED 灯珠的滚动效果，从低亮色到高亮色交替显示，适用于动态展示效果。",
                       PropertyList({Property("low_red", kPropertyTypeInteger, 0, 255),
                                     Property("low_green", kPropertyTypeInteger, 0, 255),
                                     Property("low_blue", kPropertyTypeInteger, 0, 255),
                                     Property("high_red", kPropertyTypeInteger, 0, 255),
                                     Property("high_green", kPropertyTypeInteger, 0, 255),
                                     Property("high_blue", kPropertyTypeInteger, 0, 255),
                                     Property("length", kPropertyTypeInteger, 4, 1, 8),
                                     Property("interval_ms", kPropertyTypeInteger, 100, 50, 1000)}),
                       [&led_controller](const PropertyList &properties) -> ReturnValue
                       {
                           int low_red = properties["low_red"].value<int>();
                           int low_green = properties["low_green"].value<int>();
                           int low_blue = properties["low_blue"].value<int>();
                           int high_red = properties["high_red"].value<int>();
                           int high_green = properties["high_green"].value<int>();
                           int high_blue = properties["high_blue"].value<int>();
                           int length = properties["length"].value<int>();
                           int interval_ms = properties["interval_ms"].value<int>();

                           StripColor low = {static_cast<uint8_t>(low_red),
                                             static_cast<uint8_t>(low_green),
                                             static_cast<uint8_t>(low_blue)};
                           StripColor high = {static_cast<uint8_t>(high_red),
                                              static_cast<uint8_t>(high_green),
                                              static_cast<uint8_t>(high_blue)};

                           led_controller.GetRgbLedStrip()->Scroll(low, high, length, interval_ms);
                           ESP_LOGI(TAG, "RGB LED scroll started - Low: (%d,%d,%d), High: (%d,%d,%d), Length: %d, Interval: %dms",
                                    low_red, low_green, low_blue,
                                    high_red, high_green, high_blue,
                                    length, interval_ms);

                           std::string response = "{\"success\":true"
                                                  ",\"low_color\":{"
                                                  "\"r\":" +
                                                  std::to_string(low_red) +
                                                  ",\"g\":" + std::to_string(low_green) +
                                                  ",\"b\":" + std::to_string(low_blue) + "},"
                                                                                         ",\"high_color\":{"
                                                                                         "\"r\":" +
                                                  std::to_string(high_red) +
                                                  ",\"g\":" + std::to_string(high_green) +
                                                  ",\"b\":" + std::to_string(high_blue) + "},"
                                                                                          ",\"length\":" +
                                                  std::to_string(length) +
                                                  ",\"interval_ms\":" + std::to_string(interval_ms) + "}";
                           return response;
                       });

    // 添加RGB LED 呼吸效果的工具函数
    mcp_server.AddTool("self.light.breathe",
                       "让所有 LED 灯珠执行呼吸效果，亮度从 0 逐渐增强到当前颜色再减弱到 0，循环往复。",
                       PropertyList({Property("breath_time_ms", kPropertyTypeInteger, 2000, 500, 10000)}),
                       [&led_controller](const PropertyList &properties) -> ReturnValue
                       {
                           int breath_time_ms = properties["breath_time_ms"].value<int>();

                           led_controller.GetRgbLedStrip()->StartBreathe(breath_time_ms);
                           ESP_LOGI(TAG, "RGB LED breathe effect started - Duration: %dms", breath_time_ms);

                           std::string response = "{\"success\":true"
                                                  ",\"breath_time_ms\":" +
                                                  std::to_string(breath_time_ms) + "}";
                           return response;
                       });

    // 添加RGB LED 开灯效果的工具函数
    mcp_server.AddTool("self.light.turn_on",
                       "执行开灯动画效果，LED 灯珠依次点亮，最终达到目标颜色。",
                       PropertyList({Property("total_time_ms", kPropertyTypeInteger, 1000, 200, 5000),
                                     Property("red", kPropertyTypeInteger, 0, 255),
                                     Property("green", kPropertyTypeInteger, 0, 255),
                                     Property("blue", kPropertyTypeInteger, 0, 255)}),
                       [&led_controller](const PropertyList &properties) -> ReturnValue
                       {
                           int total_time_ms = properties["total_time_ms"].value<int>();
                           int red = properties["red"].value<int>();
                           int green = properties["green"].value<int>();
                           int blue = properties["blue"].value<int>();

                           StripColor color = {static_cast<uint8_t>(red),
                                               static_cast<uint8_t>(green),
                                               static_cast<uint8_t>(blue)};

                           led_controller.GetRgbLedStrip()->TurnOnStrip(total_time_ms, color);
                           ESP_LOGI(TAG, "RGB LED turn-on sequence started - Time: %dms, Color: R:%d, G:%d, B:%d",
                                    total_time_ms, red, green, blue);

                           std::string response = "{\"success\":true"
                                                  ",\"total_time_ms\":" +
                                                  std::to_string(total_time_ms) +
                                                  ",\"red\":" + std::to_string(red) +
                                                  ",\"green\":" + std::to_string(green) +
                                                  ",\"blue\":" + std::to_string(blue) + "}";
                           return response;
                       });

    // 添加RGB LED 关灯效果的工具函数
    mcp_server.AddTool("self.light.turn_off",
                       "执行关灯动画效果，LED 灯珠亮度逐渐减弱直至完全熄灭。",
                       PropertyList({Property("fade_time_ms", kPropertyTypeInteger, 1000, 200, 5000)}),
                       [&led_controller](const PropertyList &properties) -> ReturnValue
                       {
                           int fade_time_ms = properties["fade_time_ms"].value<int>();

                           led_controller.GetRgbLedStrip()->TurnOffStrip(fade_time_ms);
                           ESP_LOGI(TAG, "RGB LED turn-off sequence started - Fade time: %dms", fade_time_ms);

                           std::string response = "{\"success\":true"
                                                  ",\"fade_time_ms\":" +
                                                  std::to_string(fade_time_ms) + "}";
                           return response;
                       });

    // 添加RGB LED 增加亮度的工具函数
    mcp_server.AddTool("self.light.increase_brightness",
                       "增加 LED 亮度等级，共 6 个等级（0-5），每次增加一级。",
                       PropertyList(),
                       [&led_controller](const PropertyList &properties) -> ReturnValue
                       {
                           led_controller.GetRgbLedStrip()->IncreaseBrightness();
                           ESP_LOGI(TAG, "RGB LED brightness increased");

                           std::string response = "{\"success\":true}";
                           return response;
                       });

    // 添加RGB LED 降低亮度的工具函数
    mcp_server.AddTool("self.light.decrease_brightness",
                       "降低 LED 亮度等级，共 6 个等级（0-5），每次降低一级。",
                       PropertyList(),
                       [&led_controller](const PropertyList &properties) -> ReturnValue
                       {
                           led_controller.GetRgbLedStrip()->DecreaseBrightness();
                           ESP_LOGI(TAG, "RGB LED brightness decreased");

                           std::string response = "{\"success\":true}";
                           return response;
                       });
}
/*
void InitializeMotorMCP(
    McpServer &mcp_server,
    FogSeekMotorController &motor_controller)
{
    // 添加直接控制电机状态的工具函数
    mcp_server.AddTool("self.motor.control_motor",
                       "打开香氛/关闭香氛，用来控制香氛的开关状态，直接控制电机的开关状态，可以启动或停止电机运行，适用于需要立即响应的场景。",
                       PropertyList({Property("state", kPropertyTypeBoolean, true)}), // true表示默认值
                       [&motor_controller](const PropertyList &properties) -> ReturnValue
                       {
                           bool state = properties["state"].value<bool>();

                           motor_controller.ControlMotor(state);

                           ESP_LOGI(TAG, "Motor control - State: %s", state ? "ON" : "OFF");

                           std::string response = "{\"success\":true"
                                                  ",\"state\":" +
                                                  std::string(state ? "true" : "false") + "}";
                           return response;
                       });

    // 添加定时运行电机的工具函数
    mcp_server.AddTool("self.motor.run_timed",
                       "让香氛工作指定时间，定时运行电机，让电机运行指定时间后自动停止，适用于控制香氛扩散时长等场景。",
                       PropertyList({Property("run_time_ms", kPropertyTypeInteger, 5000, 100, 60000)}), // 默认5秒，范围100ms到60秒
                       [&motor_controller](const PropertyList &properties) -> ReturnValue
                       {
                           int run_time_ms = properties["run_time_ms"].value<int>();
                           if (run_time_ms <= 0)
                           {
                               run_time_ms = 5000; // 默认5秒
                           }

                           motor_controller.RunMotorTimed(run_time_ms);

                           ESP_LOGI(TAG, "Motor timed run - Duration: %d ms", run_time_ms);

                           std::string response = "{\"success\":true"
                                                  ",\"run_time_ms\":" +
                                                  std::to_string(run_time_ms) + "}";
                           return response;
                       });
}

void InitializeFragranceMCP(
    McpServer &mcp_server,
    FragranceController &fragrance_controller)
{
    // 添加设置工作模式的工具函数
    mcp_server.AddTool("self.fragrance.set_work_mode",
                       "设置香氛扩散的工作模式，此模式下电机将以固定间隔运行，适合提高注意力和工作效率的场景。",
                       PropertyList(),
                       [&fragrance_controller](const PropertyList &properties) -> ReturnValue
                       {
                           fragrance_controller.SetMode(FragranceController::Mode::WORK_MODE);

                           ESP_LOGI(TAG, "Fragrance controller set to WORK_MODE");

                           std::string result = "{\"success\":true,\"mode\":\"work\"}";
                           return result;
                       });

    // 添加设置助眠模式的工具函数
    mcp_server.AddTool("self.fragrance.set_sleep_aid_mode",
                       "设置香氛扩散器为助眠模式，此模式下电机将以舒缓节奏运行，适合放松和睡眠辅助的场景。",
                       PropertyList(),
                       [&fragrance_controller](const PropertyList &properties) -> ReturnValue
                       {
                           fragrance_controller.SetMode(FragranceController::Mode::SLEEP_AID_MODE);

                           ESP_LOGI(TAG, "Fragrance controller set to SLEEP_AID_MODE");

                           std::string result = "{\"success\":true,\"mode\":\"sleep_aid\"}";
                           return result;
                       });

    // 添加设置减压模式的工具函数
    mcp_server.AddTool("self.fragrance.set_stress_relief_mode",
                       "设置香氛扩散器为减压模式，此模式下电机将以放松节奏运行，适合缓解压力和放松心情的场景。",
                       PropertyList(),
                       [&fragrance_controller](const PropertyList &properties) -> ReturnValue
                       {
                           fragrance_controller.SetMode(FragranceController::Mode::STRESS_RELIEF_MODE);

                           ESP_LOGI(TAG, "Fragrance controller set to STRESS_RELIEF_MODE");

                           std::string result = "{\"success\":true,\"mode\":\"stress_relief\"}";
                           return result;
                       });

    // 添加关闭香氛的工具函数
    mcp_server.AddTool("self.fragrance.turn_off",
                       "关闭香氛扩散器，停止电机运行并关闭相关灯光效果。",
                       PropertyList(),
                       [&fragrance_controller](const PropertyList &properties) -> ReturnValue
                       {
                           fragrance_controller.SetMode(FragranceController::Mode::OFF_MODE);

                           ESP_LOGI(TAG, "Fragrance controller turned OFF");

                           std::string result = "{\"success\":true,\"mode\":\"off\"}";
                           return result;
                       });

    // 添加设置香氛浓度的工具函数
    mcp_server.AddTool("self.fragrance.set_intensity",
                       "设置香氛扩散器的浓度级别，高浓度提供更强的香味扩散，低浓度提供温和的香味扩散。",
                       PropertyList({Property("level", kPropertyTypeString, std::string("low"))}), // 默认值为"low"
                       [&fragrance_controller](const PropertyList &properties) -> ReturnValue
                       {
                           std::string level = properties["level"].value<std::string>();

                           if (level == "high")
                           {
                               fragrance_controller.SetIntensityHigh();
                               ESP_LOGI(TAG, "Fragrance intensity set to HIGH");
                           }
                           else if (level == "low")
                           {
                               fragrance_controller.SetIntensityLow();
                               ESP_LOGI(TAG, "Fragrance intensity set to LOW");
                           }
                           else
                           {
                               std::string error_result = "{\"success\":false,\"error\":\"Invalid level, use 'high' or 'low'\"}";
                               return error_result;
                           }

                           std::string result = "{\"success\":true,\"level\":\"" + level + "\"}";
                           return result;
                       });
}

void InitializeLightPanelMCP(
    McpServer &mcp_server,
    std::function<void(bool)> set_light_state_func)
{
    // 添加控制灯光板开关的工具函数
    mcp_server.AddTool("self.light_panel.set_state",
                       "控制灯光板的开关状态，适用于控制背光或照明面板的场景。",
                       PropertyList({Property("state", kPropertyTypeBoolean, false)}),
                       [set_light_state_func](const PropertyList &properties) -> ReturnValue
                       {
                           bool state = properties["state"].value<bool>();

                           // 调用传入的回调函数来设置灯光状态
                           set_light_state_func(state);

                           ESP_LOGI(TAG, "Light panel control - State: %s", state ? "ON" : "OFF");

                           std::string response = "{\"success\":true"
                                                  ",\"state\":" +
                                                  std::string(state ? "true" : "false") + "}";
                           return response;
                       });*/