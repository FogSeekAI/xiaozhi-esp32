#ifndef _FOGSEEK_MCP_TOOLS_H_
#define _FOGSEEK_MCP_TOOLS_H_

#include "mcp_server.h"
#include "led/gpio_led.h"
#include "led/circular_strip.h" // 添加CircularStrip头文件
#include "led_controller.h"     // 添加LED控制器头文件
#include "motor_controller.h"   // 添加电机控制器头文件
#include "power_manager.h"
#include "fragrance_controller.h" // 添加香氛控制器头文件

/**
 * @brief 初始化灯光控制 MCP 工具函数
 * @param mcp_server MCP 服务器实例
 * @param cold_light 冷色灯控制实例
 * @param warm_light 暖色灯控制实例
 * @param cold_light_state 冷色灯状态引用
 * @param warm_light_state 暖色灯状态引用
 */
void InitializeLightMCP(
    McpServer &mcp_server,
    GpioLed *cold_light,
    GpioLed *warm_light,
    bool cold_light_state,
    bool warm_light_state);

/**
 * @brief 初始化RGB LED控制 MCP 工具函数
 * @param mcp_server MCP 服务器实例
 * @param led_controller LED控制器实例
 */
void InitializeRgbLedMCP(
    McpServer &mcp_server,
    FogSeekLedController &led_controller);

/**
 * @brief 初始化电机控制 MCP 工具函数
 * @param mcp_server MCP 服务器实例
 * @param motor_controller 电机控制器实例
 */
void InitializeMotorMCP(
    McpServer &mcp_server,
    FogSeekMotorController &motor_controller);

/**
 * @brief 初始化系统控制 MCP 工具函数
 * @param mcp_server MCP 服务器实例
 * @param power_manager 电源管理器实例
 */
void InitializeSystemMCP(
    McpServer &mcp_server,
    FogSeekPowerManager &power_manager);

/**
 * @brief 初始化香氛控制 MCP 工具函数
 * @param mcp_server MCP 服务器实例
 * @param fragrance_controller 香氛控制器实例
 */
void InitializeFragranceMCP(
    McpServer &mcp_server,
    FragranceController &fragrance_controller);

#endif // _FOGSEEK_MCP_TOOLS_H_