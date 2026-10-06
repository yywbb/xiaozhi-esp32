#ifndef __PCA9685_SERVO_H__
#define __PCA9685_SERVO_H__

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdint>

#define DOG_SERVO_TAG "DogServo"

// PCA9685 16 通道 PWM 板，按 50Hz 舵机模式初始化。
//
// 与 main/boards/common/motor_controller.h 里的履带驱动同源，区别只有两点：
//   1. 预分频按 50Hz 算（121），电机那版是 1526Hz（3），两者不能共存于同一块芯片
//   2. 输出的是舵机脉宽，不是电机占空比
//
// 接线（与履带挖掘机一致，可直接换插）：
//   板子 VCC -> 3.3V（I2C 逻辑电平跟着 VCC 走，接 5V 会让 3.3V 的 SDA/SCL 处于门槛边缘）
//   板子 V+  -> 舵机独立 5V（3A 以上），千万不要从 ESP32 的 5V/3V3 脚取电
//   GND 与 ESP32 共地
//   ch0~ch3 -> 左前 / 左后 / 右前 / 右后腿舵机信号线
class DogServoBus {
private:
    // PCA9685 寄存器
    static constexpr uint8_t kRegMode1 = 0x00;
    static constexpr uint8_t kRegMode2 = 0x01;
    static constexpr uint8_t kRegLed0OnL = 0x06;
    static constexpr uint8_t kRegPrescale = 0xFE;
    static constexpr uint8_t kMode1Ai = 0x20;      // auto-increment
    static constexpr uint8_t kMode1Sleep = 0x10;
    static constexpr uint8_t kMode2Outdrv = 0x04;  // totem-pole 输出，舵机要推挽驱动

    static constexpr uint32_t kOscClockHz = 25000000;
    static constexpr uint16_t kTickMax = 4095;     // 12 位计数器

    i2c_master_bus_handle_t bus_ = nullptr;
    i2c_master_dev_handle_t dev_ = nullptr;
    bool present_ = false;
    uint8_t addr_ = 0;

    DogServoBus() = default;

    bool WriteRegs(uint8_t reg, const uint8_t* data, size_t len) {
        if (dev_ == nullptr) {
            return false;
        }
        uint8_t buf[8];
        buf[0] = reg;
        for (size_t i = 0; i < len; i++) {
            buf[1 + i] = data[i];
        }
        return i2c_master_transmit(dev_, buf, len + 1, 100) == ESP_OK;
    }

    static uint8_t PrescaleFor(uint16_t freq_hz) {
        // prescale = round(osc / (4096 * freq)) - 1
        uint32_t prescale = (kOscClockHz / (4096UL * freq_hz));
        if (prescale == 0) {
            prescale = 1;
        }
        return static_cast<uint8_t>(prescale - 1);
    }

public:
    static DogServoBus& GetInstance() {
        static DogServoBus instance;
        return instance;
    }

    DogServoBus(const DogServoBus&) = delete;
    DogServoBus& operator=(const DogServoBus&) = delete;

    // 初始化 I2C 与 PCA9685。找不到芯片时不报错，只置 present_=false，
    // 这样没接硬件的板子也能正常启动、连网、跑 AI 对话。
    bool Init(gpio_num_t sda, gpio_num_t scl, uint8_t addr, uint16_t freq_hz) {
        addr_ = addr;
        if (bus_ != nullptr) {
            present_ = true;
            return true;
        }

        const i2c_master_bus_config_t bus_config = {
            .i2c_port = 1,  // port 0 留给摄像头的 SCCB
            .sda_io_num = sda,
            .scl_io_num = scl,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags = {
                .enable_internal_pullup = true,
            },
        };
        if (i2c_new_master_bus(&bus_config, &bus_) != ESP_OK) {
            ESP_LOGW(DOG_SERVO_TAG, "I2C 总线初始化失败");
            bus_ = nullptr;
            return false;
        }

        const i2c_device_config_t dev_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr,
            .scl_speed_hz = 100000,
            .scl_wait_us = 0,
            .flags = {
                .disable_ack_check = false,
            },
        };
        if (i2c_master_bus_add_device(bus_, &dev_config, &dev_) != ESP_OK) {
            ESP_LOGW(DOG_SERVO_TAG, "PCA9685 设备注册失败");
            dev_ = nullptr;
            return false;
        }

        if (i2c_master_probe(bus_, addr, 100) != ESP_OK) {
            ESP_LOGW(DOG_SERVO_TAG,
                     "在 0x%02x 没找到 PCA9685（没接舵机板？先按无舵机模式继续）",
                     addr);
            present_ = false;
            return false;
        }

        // 标准初始化顺序：sleep -> 写预分频 -> 唤醒 -> 推挽输出
        const uint8_t prescale = PrescaleFor(freq_hz);
        uint8_t mode1 = kMode1Sleep | kMode1Ai;
        WriteRegs(kRegMode1, &mode1, 1);
        WriteRegs(kRegPrescale, &prescale, 1);
        mode1 = kMode1Ai;
        WriteRegs(kRegMode1, &mode1, 1);
        const uint8_t mode2 = kMode2Outdrv;
        WriteRegs(kRegMode2, &mode2, 1);

        vTaskDelay(pdMS_TO_TICKS(1));

        // 所有通道先输出 0，避免上电瞬间乱抽
        for (int ch = 0; ch < 16; ch++) {
            SetChannelOff(ch, 0);
        }

        present_ = true;
        ESP_LOGI(DOG_SERVO_TAG, "PCA9685 就绪: 地址 0x%02x, %uHz, 预分频 %u", addr,
                 (unsigned)freq_hz, (unsigned)prescale);
        return true;
    }

    bool IsPresent() const { return present_; }

    // off_ticks: 0~4095，对应 0~20ms 一个周期内的翻转点
    bool SetChannelOff(uint8_t channel, uint16_t off_ticks) {
        if (!present_ || channel > 15) {
            return false;
        }
        if (off_ticks > kTickMax) {
            off_ticks = kTickMax;
        }
        const uint8_t reg = kRegLed0OnL + 4 * channel;
        const uint8_t data[4] = {
            0, 0,  // ON_L, ON_H = 0
            static_cast<uint8_t>(off_ticks & 0xFF),
            static_cast<uint8_t>(off_ticks >> 8),
        };
        return WriteRegs(reg, data, 4);
    }

    // 按舵机脉宽设置（500~2500us，20ms 周期）
    bool SetChannelPulseUs(uint8_t channel, uint16_t pulse_us) {
        const float period_us = 1000000.0f / 50.0f;
        uint16_t ticks = static_cast<uint16_t>(pulse_us * 4096.0f / period_us);
        return SetChannelOff(channel, ticks);
    }

    uint8_t GetAddress() const { return addr_; }
};

#endif  // __PCA9685_SERVO_H__
