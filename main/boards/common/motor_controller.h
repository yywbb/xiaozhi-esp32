#ifndef __MOTOR_CONTROLLER_H__
#define __MOTOR_CONTROLLER_H__

#include "mcp_server.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <driver/ledc.h>

#include <string>

#define MOTOR_TAG "Motor"

// One bidirectional DC motor driven by one channel of an L9110S_FOUR board.
// NOTE: this 4-channel module pulls IA/IB HIGH via onboard 10k resistor
// networks, so its inputs are ACTIVE-LOW:
//   0 / 1 -> run forward    1 / 0 -> run backward
//   1 / 1 -> brake (idle)   0 / 0 -> coast
// PWM is therefore inverted: the LOW fraction of the waveform is the power
// fraction. Forward and backward share the SAME speed->duty mapping.
class MotorController {
private:
    gpio_num_t pin_fwd_;
    gpio_num_t pin_rev_;
    esp_timer_handle_t stop_timer_ = nullptr;
    static inline MotorController* instance_ = nullptr;

    static void StopTimerCallback(void* arg) {
        static_cast<MotorController*>(arg)->Stop();
    }

    void ConfigureChannel(ledc_channel_t channel, gpio_num_t pin) {
        const ledc_channel_config_t channel_config = {
            .gpio_num = pin,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = channel,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER_1,  // timer 0 is used by the LCD backlight
            .duty = 0,
            .hpoint = 0,
            .flags = {
                .output_invert = 0,
            },
        };
        ESP_ERROR_CHECK(ledc_channel_config(&channel_config));
    }

    void SetDuty(ledc_channel_t channel, uint32_t duty) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, channel);
    }

    // Active-low: register duty is the HIGH (inactive) fraction, so it
    // decreases as speed grows. 10-bit resolution.
    static uint32_t SpeedToDuty(int speed) {
        return 1023u - (1023u * speed) / 100u;
    }

    static constexpr uint32_t kLevelHigh = 1023u;  // inactive (brake/idle)

    // direction: 1 forward, -1 backward, 0 brake stop
    void Run(int direction, int speed) {
        uint32_t duty = SpeedToDuty(speed);
        if (direction > 0) {
            SetDuty(LEDC_CHANNEL_1, duty);       // IA1 PWM (active low)
            SetDuty(LEDC_CHANNEL_2, kLevelHigh); // IA2 held high
        } else if (direction < 0) {
            SetDuty(LEDC_CHANNEL_1, kLevelHigh);
            SetDuty(LEDC_CHANNEL_2, duty);
        } else {
            SetDuty(LEDC_CHANNEL_1, kLevelHigh); // both high = brake
            SetDuty(LEDC_CHANNEL_2, kLevelHigh);
        }
    }

    void Stop() {
        Run(0, 0);
        ESP_LOGI(MOTOR_TAG, "Motor stopped");
    }

public:
    static MotorController* GetInstance() { return instance_; }

    // Shared by the MCP tool and the LAN HTTP API.
    // On success reply contains a human-readable result. Returns false and
    // fills reply with an error message for unknown actions.
    bool Drive(const std::string& action, int speed, int duration_ms,
               std::string& reply) {
        int direction = 0;
        if (action == "forward") { direction = 1; }
        else if (action == "backward") { direction = -1; }
        else if (action == "stop") { direction = 0; }
        else {
            reply = "unknown action: " + action +
                    " (expected forward/backward/stop)";
            return false;
        }

        esp_timer_stop(stop_timer_);
        Stop();
        if (direction == 0) {
            reply = "car stopped";
            return true;
        }

        Run(direction, speed);
        esp_timer_start_once(stop_timer_, (uint64_t)duration_ms * 1000);
        ESP_LOGI(MOTOR_TAG, "Action: %s, speed: %d, duration: %dms",
                 action.c_str(), speed, duration_ms);
        reply = std::string("car ") + action + " for " +
                std::to_string(duration_ms) + "ms";
        return true;
    }

    MotorController(gpio_num_t pin_fwd, gpio_num_t pin_rev)
        : pin_fwd_(pin_fwd), pin_rev_(pin_rev) {
        instance_ = this;
        // 20kHz (ultrasonic, no whine), 10-bit duty
        const ledc_timer_config_t timer_config = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_10_BIT,
            .timer_num = LEDC_TIMER_1,
            .freq_hz = 20000,
            .clk_cfg = LEDC_AUTO_CLK,
            .deconfigure = false,
        };
        ESP_ERROR_CHECK(ledc_timer_config(&timer_config));
        ConfigureChannel(LEDC_CHANNEL_1, pin_fwd_);
        ConfigureChannel(LEDC_CHANNEL_2, pin_rev_);
        Stop();

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
            Property("speed", kPropertyTypeInteger, 70, 30, 100),
        });
        mcp_server.AddTool("self.car.drive",
            "Control a toy car's single drive motor. Action must be one of: "
            "\"forward\", \"backward\", or \"stop\". Every move runs for "
            "duration_ms (default 1500ms, hard max 5000ms) and then stops "
            "automatically for safety. Call \"stop\" immediately when the user "
            "asks to stop. speed is 30-100, default 70.",
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
        ESP_LOGI(MOTOR_TAG, "Motor controller ready: fwd=%d rev=%d",
                 pin_fwd_, pin_rev_);
    }

    ~MotorController() {
        instance_ = nullptr;
        if (stop_timer_ != nullptr) {
            esp_timer_stop(stop_timer_);
            esp_timer_delete(stop_timer_);
        }
        Stop();
    }
};

#endif // __MOTOR_CONTROLLER_H__
