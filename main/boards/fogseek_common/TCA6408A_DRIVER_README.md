# TCA6408A I/O 扩展器驱动

## 概述

本目录包含 TCA6408A I/O 扩展器的完整驱动程序，用于 Edge V4.2 开发板。TCA6408A 通过 I2C 总线为 MCU 提供额外的 8 个可配置 GPIO，并具备双向电压电平转换能力。

## 文件结构

```
fogseek_common/
├── tca6408a_io_expander.h          # TCA6408A 底层驱动头文件
├── tca6408a_io_expander.c          # TCA6408A 底层驱动实现
├── tca6408a_power_manager.h        # 基于 IO 扩展器的电源管理器（独立类）
├── tca6408a_power_manager.cc       # 基于 IO 扩展器的电源管理器实现
├── tca6408a_led_controller.h       # 基于 IO 扩展器的 LED 控制器（独立类）
└── tca6408a_led_controller.cc      # 基于 IO 扩展器的 LED 控制器实现
```

## 设计说明

### 解耦设计

本项目采用**完全解耦**的设计方式，提供两套独立的控制系统：

1. **原生 GPIO 控制** - 使用原有的 `power_manager.h/cc` 和 `led_controller.h/cc`
2. **IO 扩展器控制** - 使用新增的 `tca6408a_*` 系列文件

两种方式**互不干扰**，可以根据硬件设计选择使用：
- 使用原生 GPIO：包含原有头文件，使用原有类
- 使用 IO 扩展器：包含新的 `tca6408a_*` 头文件，使用新的类

### 核心优势

✅ **代码简洁清晰** - 每个类职责单一，易于理解和维护  
✅ **完全解耦** - 不影响原有代码，两种模式可共存  
✅ **即插即用** - 只需更换包含的头文件和类即可切换模式  
✅ **向后兼容** - 保留原有所有功能和实现  

## 硬件连接（Edge V4.2）

### TCA6408A 引脚映射

| TCA6408A 引脚 | 功能 | 默认方向 | 备注 |
|----------|------|-----|------|
| P0 | LCD 背光控制 | 输出 | 控制背光开关 |
| P1 | 音频功放使能 | 输出 | 控制功放开关 |
| P2 | 红色 LED | 输出 | 控制红灯亮灭 |
| P3 | 绿色 LED | 输出 | 控制绿灯亮灭 |
| P4 | 控制按键 | 输入 | 需启用上拉，用于中断检测 |
| P5 | 电源保持 | 输出 | 控制电源维持 |
| P6 | 充电完成检测 | 输入 | 检测充电 IC 状态 |
| P7 | 充电中检测 | 输入 | 检测充电 IC 状态 |

### I2C 地址配置

ADDR 引脚决定 I2C 从机地址：
- **接地 (GND)**: 7 位地址 = `0x20`
- **接 VCCP**: 7 位地址 = `0x21`

Edge V4.2 默认 ADDR 接地，使用地址 **0x20**。

## API 参考

### 1. 底层驱动 API（tca6408a_io_expander.h）

#### 初始化

```c
tca6408a_config_t config = {
    .i2c_bus = i2c_bus,
    .i2c_address = 0x20,
    .int_gpio = GPIO_NUM_NC,
    .reset_gpio = GPIO_NUM_NC
};

tca6408a_handle_t io_expander;
esp_err_t ret = tca6408a_init(&io_expander, &config);
```

#### GPIO 控制

```c
// 设置单个 GPIO 输出
tca6408a_set_gpio_level(&io_expander, TCA6408A_GPIO_P2, 1);

// 读取单个 GPIO 输入
uint8_t level;
tca6408a_get_gpio_level(&io_expander, TCA6408A_GPIO_P4, &level);
```

详细 API 请参考 [tca6408a_io_expander.h](file:///home/pawnma/xiaozhi-esp32/main/boards/fogseek_common/tca6408a_io_expander.h)

---

### 2. 电源管理器（tca6408a_power_manager.h）

#### 类说明

`TCA6408APowerManager` 是独立的电源管理类，通过 TCA6408A 控制电源相关功能。

#### 使用示例

```cpp
#include "tca6408a_power_manager.h"

// 创建实例
TCA6408APowerManager power_manager;

// 配置引脚
TCA6408APowerManager::power_pin_config_t pin_config = {
    .charging_gpio = P7,      // 充电中检测（原生 GPIO）
    .charge_done_gpio = P6,   // 充电完成检测（原生 GPIO）
    .adc_gpio = BATTERY_ADC_GPIO
};

// 初始化
esp_err_t ret = power_manager.Initialize(&pin_config, i2c_bus, 0x20);

// 开机
power_manager.PowerOn();

// 关机
power_manager.PowerOff();

// 获取状态
auto state = power_manager.GetPowerState();
bool is_on = power_manager.IsPowerOn();
uint8_t battery = power_manager.ReadBatteryLevel();

// 设置回调
power_manager.SetPowerStateCallback([](TCA6408APowerManager::PowerState state) {
    ESP_LOGI("TAG", "Power state changed: %d", static_cast<int>(state));
});
```

#### 主要方法

| 方法 | 说明 |
|-----|------|
| `Initialize()` | 初始化电源管理器 |
| `PowerOn()` | 开机 |
| `PowerOff()` | 关机 |
| `GetPowerState()` | 获取电源状态 |
| `IsPowerOn()` | 是否开机 |
| `IsUsbPowered()` | 是否 USB 供电 |
| `ReadBatteryLevel()` | 读取电池电量 |
| `SetPowerStateCallback()` | 设置状态变化回调 |

---

### 3. LED 控制器（tca6408a_led_controller.h）

#### 类说明

`TCA6408ALedController` 是独立的 LED 控制类，通过 TCA6408A 控制红绿灯，原生 GPIO 控制其他灯光。

#### 使用示例

```cpp
#include "tca6408a_led_controller.h"

// 创建实例
TCA6408ALedController led_controller;

// 配置引脚
TCA6408ALedController::led_pin_config_t pin_config = {
    .rgb_gpio = -1,           // 不使用 RGB 灯带
    .rgb_num_leds = 0,
    .cold_light_gpio = -1,    // 不使用冷暖色灯
    .warm_light_gpio = -1
};

// 初始化
esp_err_t ret = led_controller.Initialize(&pin_config, i2c_bus, 0x20);

// 手动控制红绿灯
led_controller.SetRedLed(true);   // 开红灯
led_controller.SetGreenLed(false); // 关绿灯

// 自动更新状态（根据设备和电源状态）
auto device_state = app.GetDeviceState();
auto power_state = power_manager.GetPowerState();
auto device_power_state = power_manager.GetDevicePowerState();
led_controller.UpdateLedStatus(device_state, power_state, device_power_state);

// 控制冷暖色灯（原生 GPIO）
led_controller.SetColdLight(true);
led_controller.SetWarmLightBrightness(50);
```

#### 主要方法

| 方法 | 说明 |
|-----|------|
| `Initialize()` | 初始化 LED 控制器 |
| `SetRedLed(bool)` | 设置红灯开关 |
| `SetGreenLed(bool)` | 设置绿灯开关 |
| `UpdateLedStatus()` | 自动更新 LED 状态 |
| `SetColdLight(bool)` | 设置冷色灯 |
| `SetWarmLight(bool)` | 设置暖色灯 |
| `SetColdLightBrightness(int)` | 设置冷色灯亮度 |
| `SetWarmLightBrightness(int)` | 设置暖色灯亮度 |

---

## 完整应用示例（fogseek-edge.cc）

```cpp
#include "tca6408a_power_manager.h"
#include "tca6408a_led_controller.h"

class FogSeekEdge : public WifiBoard
{
private:
    TCA6408APowerManager power_manager_;
    TCA6408ALedController led_controller_;
    i2c_master_bus_handle_t i2c_bus_ = nullptr;

    void InitializeI2c()
    {
        // 创建 I2C 总线
        i2c_master_bus_config_t i2c_bus_cfg = {...};
        i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_);
    }

    void InitializePowerManager()
    {
        TCA6408APowerManager::power_pin_config_t pin_config = {
            .charging_gpio = P7,
            .charge_done_gpio = P6,
            .adc_gpio = BATTERY_ADC_GPIO
        };
        
        esp_err_t ret = power_manager_.Initialize(&pin_config, i2c_bus_, 0x20);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize power manager");
        }
    }

    void InitializeLedController()
    {
        TCA6408ALedController::led_pin_config_t pin_config = {};
        
        esp_err_t ret = led_controller_.Initialize(&pin_config, i2c_bus_, 0x20);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize LED controller");
        }
    }

public:
    FogSeekEdge()
    {
        InitializeI2c();
        InitializePowerManager();
        InitializeLedController();
        
        // 设置电源状态回调
        power_manager_.SetPowerStateCallback([this](auto state) {
            auto device_state = Application::GetInstance().GetDeviceState();
            auto device_power_state = power_manager_.GetDevicePowerState();
            led_controller_.UpdateLedStatus(device_state, state, device_power_state);
        });
    }

    void PowerOn()
    {
        power_manager_.PowerOn();
        
        // 更新 LED
        auto device_state = Application::GetInstance().GetDeviceState();
        auto power_state = power_manager_.GetPowerState();
        auto device_power_state = power_manager_.GetDevicePowerState();
        led_controller_.UpdateLedStatus(device_state, power_state, device_power_state);
    }

    void PowerOff()
    {
        power_manager_.PowerOff();
        
        // 更新 LED
        auto device_state = Application::GetInstance().GetDeviceState();
        auto power_state = power_manager_.GetPowerState();
        auto device_power_state = power_manager_.GetDevicePowerState();
        led_controller_.UpdateLedStatus(device_state, power_state, device_power_state);
    }
};
```

## 两种模式对比

| 特性 | 原生 GPIO 模式 | IO 扩展器模式 |
|-----|--------------|-------------|
| 使用文件 | `power_manager.h/cc`<br>`led_controller.h/cc` | `tca6408a_power_manager.h/cc`<br>`tca6408a_led_controller.h/cc` |
| 类名 | `FogSeekPowerManager`<br>`FogSeekLedController` | `TCA6408APowerManager`<br>`TCA6408ALedController` |
| 红绿灯控制 | 原生 GPIO + PWM | TCA6408A（数字输出） |
| 冷暖色灯 | 原生 GPIO + PWM | 原生 GPIO + PWM |
| RGB 灯带 | 原生 GPIO | 原生 GPIO |
| 电源保持 | 原生 GPIO | TCA6408A P5 |
| 充电检测 | 原生 GPIO | TCA6408A P6, P7 |
| PWM 支持 | ✅ 完整支持 | ❌ 红绿灯仅数字开关 |
| 代码复杂度 | 中等 | 低（职责分离） |
| 适用场景 | 需要 PWM 调光 | 简化设计，节省 GPIO |

## 注意事项

### 1. 选择使用模式

⚠️ **重要**：原生 GPIO 模式和 IO 扩展器模式**不能混用**！

- 如果板级设计使用 TCA6408A，请**只包含**`tca6408a_*.h` 文件
- 如果板级设计使用原生 GPIO，请**只包含**原有的 `power_manager.h` 和 `led_controller.h`

### 2. 避免冲突

```cpp
// ❌ 错误示范 - 不要同时使用两种方式
#include "power_manager.h"              // 原生
#include "tca6408a_power_manager.h"     // IO 扩展器

// ✅ 正确做法 - 二选一
// 方案 A：原生 GPIO
#include "power_manager.h"
FogSeekPowerManager power_manager;

// 方案 B：IO 扩展器
#include "tca6408a_power_manager.h"
TCA6408APowerManager power_manager;
```

### 3. 硬件限制

- **红绿灯 PWM**：IO 扩展器模式下不支持 PWM 呼吸效果，只能数字开关
- **充电检测**：P6/P7 通过 TCA6408A 读取，响应速度略慢于原生 GPIO
- **I2C 依赖**：需要先初始化 I2C 总线才能使用 IO 扩展器

### 4. 初始化顺序

```cpp
void Initialize()
{
    // 1. 先初始化 I2C
    InitializeI2c();
    
    // 2. 再初始化 IO 扩展器设备
    power_manager_.Initialize(...);  // 会自动初始化 TCA6408A
    led_controller_.Initialize(...); // 复用已初始化的 TCA6408A
    
    // 3. 最后初始化其他外设
    InitializeAudioAmplifier();
}
```

## 技术特性

### 寄存器映射

| 寄存器 | 地址 | R/W | 默认值 |
|-------|------|-----|--------|
| INPUT | 0x00 | R | 引脚状态 |
| OUTPUT | 0x01 | R/W | 0xFF |
| POLARITY | 0x02 | R/W | 0x00 |
| CONFIG | 0x03 | R/W | 0xFF |

### 电气特性

- **工作电压**: 1.65V - 5.5V
- **I2C 速率**: 最高 400kHz
- **灌电流**: 25mA（可直接驱动 LED）
- **拉电流**: 10mA

## 故障排查

### 问题 1：无法初始化

**现象**：`Failed to initialize TCA6408A`

**解决方法**：
1. 检查 I2C 总线是否正确初始化
2. 确认 I2C 地址（0x20 或 0x21）
3. 测量 VCCI 和 VCCP 供电是否正常

### 问题 2：GPIO 控制无效

**现象**：设置输出但 LED 不亮

**解决方法**：
1. 确认引脚方向配置正确（输出模式）
2. 检查 OUTPUT 寄存器写入是否成功
3. 验证外部电路连接

### 问题 3：与原生 GPIO 冲突

**现象**：编译错误或运行时异常

**解决方法**：
1. 检查是否同时包含了两种模式的头文件
2. 确保只使用一种控制方式
3. 清理构建缓存后重新编译

## 版本历史

- **v2.0** (2024): 重构版本，完全解耦设计
  - 创建独立的 `TCA6408APowerManager` 类
  - 创建独立的 `TCA6408ALedController` 类
  - 与原有代码完全分离，互不干扰
  
- **v1.0** (2024): 初始版本，集成在原有类中
  - 在 `FogSeekPowerManager` 中添加 IO 扩展器支持
  - 在 `FogSeekLedController` 中添加 IO 扩展器支持

## 相关文档

- [TCA6408A 数据手册](https://www.ti.com/product/TCA6408A)
- Edge V4.2 原理图
- ESP-IDF I2C 驱动文档
