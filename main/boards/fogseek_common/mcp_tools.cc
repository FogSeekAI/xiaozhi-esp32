#include "mcp_tools.h"
#include "fragrance_controller.h" // 添加香氛控制器头文件
#include "led_controller.h"       // 添加LED控制器头文件
#include <esp_log.h>
#include <cJSON.h>
#include "mcp_server.h"
#include "application.h"
#include "board.h"
#include "display.h"
#include "esp_sleep.h"
#include "power_manager.h"

static const char *TAG = "FogSeekMCPTools";

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

void InitializeLightMCP(
    McpServer &mcp_server,
    GpioLed *cold_light,
    GpioLed *warm_light,
    bool cold_light_state,
    bool warm_light_state)
{
    // 添加获取当前灯状态的工具函数
    mcp_server.AddTool("self.light.get_status",
                       "获取当前灯的状态",
                       PropertyList(),
                       [cold_light_state, warm_light_state](const PropertyList &properties) -> ReturnValue
                       {
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
                       [cold_light, warm_light, &cold_light_state, &warm_light_state](const PropertyList &properties) -> ReturnValue
                       {
                           // 使用operator[]而不是at()访问属性
                           int cold_brightness = properties["cold_brightness"].value<int>();
                           int warm_brightness = properties["warm_brightness"].value<int>();

                           cold_light->SetBrightness(cold_brightness);
                           warm_light->SetBrightness(warm_brightness);

                           // 只有在亮度大于0时才开启灯光
                           if (cold_brightness > 0)
                           {
                               cold_light->TurnOn();
                           }
                           else
                           {
                               cold_light->TurnOff();
                           }

                           if (warm_brightness > 0)
                           {
                               warm_light->TurnOn();
                           }
                           else
                           {
                               warm_light->TurnOff();
                           }

                           // 更新状态
                           cold_light_state = cold_brightness > 0;
                           warm_light_state = warm_brightness > 0;

                           ESP_LOGI(TAG, "Color temperature set - Cold: %d%%, Warm: %d%%",
                                    cold_brightness, warm_brightness);

                           // 使用字符串拼接方式返回JSON - 项目中最标准的做法
                           std::string result = "{\"success\":true"
                                                ",\"cold_brightness\":" +
                                                std::to_string(cold_brightness) +
                                                ",\"warm_brightness\":" + std::to_string(warm_brightness) + "}";
                           return result;
                       });
}

void InitializeRgbLedMCP(
    McpServer &mcp_server,
    FogSeekLedController &led_controller)
{
    // 添加RGB LED跑马灯效果的工具函数
    mcp_server.AddTool("self.light.run_marquee_lights",
                       "运行RGB LED灯带的跑马灯效果，在指定时间内依次点亮所有灯，营造氛围感。大模型应该根据用户的需求和场景描述，选择合适的跑马灯持续时间。",
                       PropertyList({Property("total_time_ms", kPropertyTypeInteger, 100, 30000)}), // 默认5秒跑马灯效果
                       [&led_controller](const PropertyList &properties) -> ReturnValue
                       {
                           int total_time_ms = properties["total_time_ms"].value<int>();
                           if (total_time_ms <= 0)
                           {
                               total_time_ms = 5000; // 默认5秒
                           }

                           bool result = led_controller.RunMarqueeLights(total_time_ms);

                           ESP_LOGI(TAG, "RGB LED marquee lights effect %s - Duration: %d ms",
                                    result ? "started" : "failed", total_time_ms);

                           std::string response = "{\"success\":" + std::string(result ? "true" : "false") +
                                                  ",\"total_time_ms\":" + std::to_string(total_time_ms) + "}";
                           return response;
                       });

    // 添加RGB LED打开灯光效果的工具函数
    mcp_server.AddTool("self.light.turn_on_lights",
                       "打开RGB LED灯带，使灯光在指定时间内从暗逐渐变亮到当前设置的亮度。",
                       PropertyList({Property("duration_ms", kPropertyTypeInteger, 100, 5000)}), // 默认1秒渐亮
                       [&led_controller](const PropertyList &properties) -> ReturnValue
                       {
                           int duration_ms = properties["duration_ms"].value<int>();
                           if (duration_ms <= 0)
                           {
                               duration_ms = 1000; // 默认1秒
                           }

                           bool result = led_controller.TurnOnRgbLights(duration_ms);

                           ESP_LOGI(TAG, "RGB LED turn on effect %s - Duration: %d ms",
                                    result ? "started" : "failed", duration_ms);

                           std::string response = "{\"success\":" + std::string(result ? "true" : "false") +
                                                  ",\"duration_ms\":" + std::to_string(duration_ms) + "}";
                           return response;
                       });

    // 添加RGB LED关闭灯光效果的工具函数
    mcp_server.AddTool("self.light.turn_off_lights",
                       "关闭RGB LED灯带，使灯光在指定时间内从亮逐渐变暗直到完全关闭。",
                       PropertyList({Property("duration_ms", kPropertyTypeInteger, 100, 5000)}), // 默认1秒渐暗
                       [&led_controller](const PropertyList &properties) -> ReturnValue
                       {
                           int duration_ms = properties["duration_ms"].value<int>();
                           if (duration_ms <= 0)
                           {
                               duration_ms = 1000; // 默认1秒
                           }

                           bool result = led_controller.TurnOffRgbLights(duration_ms);

                           ESP_LOGI(TAG, "RGB LED turn off effect %s - Duration: %d ms",
                                    result ? "started" : "failed", duration_ms);

                           std::string response = "{\"success\":" + std::string(result ? "true" : "false") +
                                                  ",\"duration_ms\":" + std::to_string(duration_ms) + "}";
                           return response;
                       });

    // 添加RGB LED增加亮度的工具函数
    mcp_server.AddTool("self.light.increase_brightness",
                       "增加RGB LED灯带的亮度一个档位，适用于需要更亮环境的场景。",
                       PropertyList(),
                       [&led_controller](const PropertyList &properties) -> ReturnValue
                       {
                           led_controller.IncreaseBrightness();

                           ESP_LOGI(TAG, "RGB LED brightness increased");

                           std::string response = "{\"success\":true}";
                           return response;
                       });

    // 添加RGB LED降低亮度的工具函数
    mcp_server.AddTool("self.light.decrease_brightness",
                       "降低RGB LED灯带的亮度一个档位，适用于需要更柔和光线的场景。",
                       PropertyList(),
                       [&led_controller](const PropertyList &properties) -> ReturnValue
                       {
                           led_controller.DecreaseBrightness();

                           ESP_LOGI(TAG, "RGB LED brightness decreased");

                           std::string response = "{\"success\":true}";
                           return response;
                       });

    // 添加RGB LED随机颜色变化的工具函数
    mcp_server.AddTool("self.light.change_random_colors",
                       "随机变化RGB LED灯带的颜色（红橙黄绿青蓝紫），适用于需要多彩氛围的场景。",
                       PropertyList(),
                       [&led_controller](const PropertyList &properties) -> ReturnValue
                       {
                           led_controller.ChangeToRandomColors();

                           ESP_LOGI(TAG, "RGB LED color changed to random color");

                           std::string response = "{\"success\":true}";
                           return response;
                       });
}

void InitializeMotorMCP(
    McpServer &mcp_server,
    FogSeekMotorController &motor_controller)
{
    // 添加直接控制电机状态的工具函数
    mcp_server.AddTool("self.motor.control_motor",
                       "直接控制电机的开关状态，可以启动或停止电机运行，适用于需要立即响应的场景。",
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
                       "定时运行电机，让电机运行指定时间后自动停止，适用于控制香氛扩散时长等场景。",
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
                       "设置香氛扩散器为工作模式，此模式下电机将以固定间隔运行，适合提高注意力和工作效率的场景。",
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