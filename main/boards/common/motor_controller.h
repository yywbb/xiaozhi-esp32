#ifndef __MOTOR_CONTROLLER_H__
#define __MOTOR_CONTROLLER_H__

#include "mcp_server.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <driver/ledc.h>
#include <driver/i2c_master.h>

#include <string>

#define MOTOR_TAG "Motor"

// Tank-style dual-track drive for a toy excavator.
//
// A PCA9685 16-channel PWM board (on its own I2C bus) generates the four
// signals for a L9110S_FOUR module:
//   PCA9685 ch0 -> A1   ch1 -> A2   = LEFT track
//   PCA9685 ch2 -> B1   ch3 -> B2   = RIGHT track
// The L9110S inputs are ACTIVE-LOW (onboard 10k pull-ups to its VCC), so a
// LOW output means "drive": per channel (I1, I2)
//   (PWM, HIGH) = forward    (HIGH, PWM) = backward    (HIGH, HIGH) = brake
// PCA9685 duty therefore carries the INACTIVE (HIGH) fraction, decreasing
// as speed grows.
// Actions:
//   forward      both tracks forward
//   backward     both tracks backward
//   left         left brake, right forward (pivot)
//   right        left forward, right brake (pivot)
//   stop         all channels brake
// Every move auto-stops after duration_ms (hard max 5s) for safety.

class MotorController {
private:
    // PCA9685 registers
    static constexpr uint8_t kRegMode1 = 0x00;
    static constexpr uint8_t kRegMode2 = 0x01;
    static constexpr uint8_t kRegLed0OnL = 0x06;
    static constexpr uint8_t kRegPrescale = 0xFE;
    static constexpr uint8_t kMode1Ai = 0x20;   // auto-increment
    static constexpr uint8_t kMode1Sleep = 0x10;
    static constexpr uint8_t kMode2Outdrv = 0x04;  // totem-pole outputs
    // 25MHz / (4096 * (prescale+1)): prescale 3 -> ~1526Hz (chip max)
    // 50 Hz is required for servos (20ms period). Track DC motors still work
    // through the L9110S at this frequency (constant duty at speed 100).
    static constexpr uint8_t kPrescale = 121;  // ~50.07 Hz

    // Excavator arm servo channel (tracks use ch0-ch3)
    static constexpr int kServoChannel = 4;
    static constexpr uint16_t kFullHigh = 4096;    // LED_FULL: always HIGH

    i2c_master_bus_handle_t bus_ = nullptr;
    i2c_master_dev_handle_t dev_ = nullptr;
    bool present_ = false;
    esp_timer_handle_t stop_timer_ = nullptr;
    static inline MotorController* instance_ = nullptr;

    static void StopTimerCallback(void* arg) {
        static_cast<MotorController*>(arg)->Stop();
    }

    bool WriteRegs(uint8_t reg, const uint8_t* data, size_t len) {
        uint8_t buf[8];
        buf[0] = reg;
        for (size_t i = 0; i < len; i++) buf[1 + i] = data[i];
        return i2c_master_transmit(dev_, buf, len + 1, 100) == ESP_OK;
    }

    // duty: 0 = always LOW (full power), 4095 = mostly HIGH, 4096 = LED_FULL
    bool SetChannel(int channel, uint16_t duty) {
        uint8_t reg = kRegLed0OnL + 4 * channel;
        uint8_t data[4] = {
            0, 0,  // ON = 0
            (uint8_t)(duty & 0xFF),
            (uint8_t)(duty >> 8),
        };
        return WriteRegs(reg, data, 4);
    }

    // Active-low module: register duty = HIGH (inactive) fraction.
    static uint16_t SpeedToDuty(int speed) {
        return 4095u - (4095u * speed) / 100u;
    }

    void Brake() {
        SetChannel(0, kFullHigh);
        SetChannel(1, kFullHigh);
        SetChannel(2, kFullHigh);
        SetChannel(3, kFullHigh);
    }

public:
    static MotorController* GetInstance() { return instance_; }

    // angle: 0-180 degrees -> 500-2500us pulse. PCA9685 counts: 4096 per
    // 20ms period, so duty = pulse_us * 4096 / 20000.
    void SetServoAngle(int channel, int angle) {
        if (!present_ || channel < 0 || channel > 15) {
            return;
        }
        if (angle < 0) angle = 0;
        if (angle > 180) angle = 180;
        uint32_t pulse_us = 500 + (2000 * angle) / 180;
        uint32_t duty = (pulse_us * 4096) / 20000;
        SetChannel(channel, (uint16_t)duty);
    }

    // direction: 1 forward, -1 backward, 0 brake
    void SetTrack(int ch_fwd, int ch_rev, int direction, int speed) {
        uint16_t duty = SpeedToDuty(speed);
        if (direction > 0) {
            SetChannel(ch_fwd, duty);
            SetChannel(ch_rev, kFullHigh);
        } else if (direction < 0) {
            SetChannel(ch_fwd, kFullHigh);
            SetChannel(ch_rev, duty);
        } else {
            SetChannel(ch_fwd, kFullHigh);
            SetChannel(ch_rev, kFullHigh);
        }
    }

    void Run(const std::string& action, int speed) {
        if (action == "forward") {
            SetTrack(0, 1, 1, speed);   // left forward
            SetTrack(2, 3, 1, speed);   // right forward
        } else if (action == "backward") {
            SetTrack(0, 1, -1, speed);
            SetTrack(2, 3, -1, speed);
        } else if (action == "left") {
            SetTrack(0, 1, 0, speed);   // left brake
            SetTrack(2, 3, 1, speed);   // right forward
        } else if (action == "right") {
            SetTrack(0, 1, 1, speed);   // left forward
            SetTrack(2, 3, 0, speed);   // right brake
        } else {  // stop
            Brake();
        }
    }

    void Stop() {
        Run("stop", 0);
        ESP_LOGI(MOTOR_TAG, "Motor stopped");
    }

    void InitPca9685(gpio_num_t sda, gpio_num_t scl, uint8_t addr) {
        // The camera SCCB driver may occupy either I2C port depending on init
        // order, so try each port and use the first free one.
        i2c_master_bus_config_t bus_config = {};
        bus_config.sda_io_num = sda;
        bus_config.scl_io_num = scl;
        bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
        bus_config.glitch_ignore_cnt = 7;
        bus_config.flags.enable_internal_pullup = true;

        bool bus_ok = false;
        for (int port = 0; port < SOC_I2C_NUM && !bus_ok; port++) {
            bus_config.i2c_port = port;
            if (i2c_new_master_bus(&bus_config, &bus_) == ESP_OK) {
                bus_ok = true;
                ESP_LOGI(MOTOR_TAG, "I2C bus on port %d", port);
            }
        }
        if (!bus_ok) {
            ESP_LOGW(MOTOR_TAG, "I2C bus init failed (all ports busy)");
            return;
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
            return;
        }
        if (i2c_master_probe(bus_, addr, 100) != ESP_OK) {
            ESP_LOGW(MOTOR_TAG, "PCA9685 not found at 0x%02x (not connected?)",
                     addr);
            return;
        }

        // Standard PCA9685 init: sleep -> set prescale -> wake -> totem pole
        uint8_t mode1 = kMode1Sleep | kMode1Ai;
        WriteRegs(kRegMode1, &mode1, 1);
        WriteRegs(kRegPrescale, &kPrescale, 1);
        mode1 = kMode1Ai;
        WriteRegs(kRegMode1, &mode1, 1);
        uint8_t mode2 = kMode2Outdrv;
        WriteRegs(kRegMode2, &mode2, 1);

        Brake();
        SetServoAngle(kServoChannel, 0);
        present_ = true;
        ESP_LOGI(MOTOR_TAG, "PCA9685 ready at 0x%02x, PWM ~50Hz", addr);
    }

    // Shared by the MCP tool and the LAN HTTP API.
    // On success reply contains a human-readable result. Returns false and
    // fills reply with an error message on unknown actions or missing board.
    bool Drive(const std::string& action, int speed, int duration_ms,
               std::string& reply) {
        if (action != "forward" && action != "backward" &&
            action != "left" && action != "right" && action != "stop") {
            reply = "unknown action: " + action +
                    " (expected forward/backward/left/right/stop)";
            return false;
        }
        if (!present_) {
            reply = "motor driver board (PCA9685) not connected";
            return false;
        }

        esp_timer_stop(stop_timer_);
        Stop();
        if (action == "stop") {
            reply = "car stopped";
            return true;
        }

        Run(action, speed);
        esp_timer_start_once(stop_timer_, (uint64_t)duration_ms * 1000);
        ESP_LOGI(MOTOR_TAG, "Action: %s, speed: %d, duration: %dms",
                 action.c_str(), speed, duration_ms);
        reply = std::string("car ") + action + " for " +
                std::to_string(duration_ms) + "ms";
        return true;
    }

    MotorController(gpio_num_t sda, gpio_num_t scl, uint8_t addr) {
        instance_ = this;
        InitPca9685(sda, scl, addr);

        const esp_timer_create_args_t timer_args = {
            .callback = StopTimerCallback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "motor_stop",
            .skip_unhandled_events = false,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &stop_timer_));

        auto& mcp_server = McpServer::GetInstance();
        PropertyList properties({
            Property("action", kPropertyTypeString).SetMaxLength(16),
            Property("duration_ms", kPropertyTypeInteger, 1500, 100, 5000),
            Property("speed", kPropertyTypeInteger, 100, 30, 100),
        });
        mcp_server.AddTool("self.car.drive",
            "Control a tank-style toy excavator's two track motors. Action "
            "must be one of: \"forward\", \"backward\", \"left\", \"right\", "
            "or \"stop\". left/right pivot around the braked track. Every "
            "move runs for duration_ms (default 1500ms, hard max 5000ms) and "
            "then stops automatically for safety. Call \"stop\" immediately "
            "when the user asks to stop. speed is 30-100, default 100.",
            properties, [this](const PropertyList& props) -> ToolResult {
                auto action = props["action"].value<std::string>();
                int duration_ms = props["duration_ms"].value<int>();
                int speed = props["speed"].value<int>();

                std::string reply;
                if (!Drive(action, speed, duration_ms, reply)) {
                    return std::unexpected(reply);
                }
                return reply;
            });
        PropertyList servo_props({
            Property("angle", kPropertyTypeInteger, 90, 0, 180),
            Property("channel", kPropertyTypeInteger, kServoChannel, 4, 15),
        });
        mcp_server.AddTool("self.servo.set_angle",
            "Set the excavator arm servo position. angle is 0-180 degrees, "
            "90 is center. channel defaults to the arm servo.",
            servo_props, [this](const PropertyList& props) -> ToolResult {
                if (!present_) {
                    return std::unexpected(
                        "servo driver board (PCA9685) not connected");
                }
                int angle = props["angle"].value<int>();
                int channel = props["channel"].value<int>();
                SetServoAngle(channel, angle);
                ESP_LOGI(MOTOR_TAG, "Servo ch%d -> %d deg", channel, angle);
                return "servo set to " + std::to_string(angle) + " degrees";
            });
        if (present_) {
            ESP_LOGI(MOTOR_TAG, "Tank controller ready (PCA9685)");
        } else {
            ESP_LOGW(MOTOR_TAG,
                     "Tank controller idle: connect the PCA9685 board and reboot");
        }
    }

    ~MotorController() {
        instance_ = nullptr;
        if (stop_timer_ != nullptr) {
            esp_timer_stop(stop_timer_);
            esp_timer_delete(stop_timer_);
        }
        if (present_) {
            Brake();
        }
    }
};

#endif // __MOTOR_CONTROLLER_H__
