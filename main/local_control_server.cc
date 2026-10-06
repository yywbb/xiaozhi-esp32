#include "local_control_server.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "local_control_page.h"
#include "settings.h"

// NOTE: display.h defines HAVE_LVGL when LVGL support is compiled in, so these
// must be included unconditionally (same pattern as mcp_server.cc); wrapping
// them in #ifdef HAVE_LVGL would leave the macro undefined for the handlers
// below, disabling screen snapshot and theme switching silently.
#include "display.h"
#include "lvgl_theme.h"
#include "motor_controller.h"

#define TAG "LocalCtrl"

#ifdef CONFIG_BOARD_TYPE_BREAD_COMPACT_WIFI_S3CAM_DOG
// Extern functions from eda_dog_controller.cc (global scope)
extern bool QueueDogAction(int action_type, int steps, int speed, int direction, int height);
extern bool IsDogBusy();

// Action type constants (mirrors eda_dog_controller.cc enum)
enum DogActionApi {
    DOG_WALK = 1, DOG_TURN = 2, DOG_SIT = 3, DOG_STAND = 4,
    DOG_STRETCH = 5, DOG_SHAKE = 6,
    DOG_LIFT_LF = 7, DOG_LIFT_LR = 8, DOG_LIFT_RF = 9, DOG_LIFT_RR = 10,
    DOG_HOME = 11, DOG_SLEEP = 12
};
#endif

namespace {

constexpr int kMaxBodyBytes = 1024;
constexpr size_t kChunkSize = 4096;

std::string ReadBody(httpd_req_t* req) {
    int total = req->content_len;
    if (total <= 0) {
        return "";
    }
    if (total > kMaxBodyBytes) {
        return "";
    }
    std::string buf(static_cast<size_t>(total), '\0');
    int offset = 0;
    while (offset < total) {
        int n = httpd_req_recv(req, buf.data() + offset, total - offset);
        if (n <= 0) {
            if (n == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return "";
        }
        offset += n;
    }
    return buf;
}

cJSON* ParseBodyJson(httpd_req_t* req) {
    auto body = ReadBody(req);
    if (body.empty()) {
        return nullptr;
    }
    return cJSON_Parse(body.c_str());
}

esp_err_t SendJson(httpd_req_t* req, const char* status, const std::string& json) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, json.c_str(), json.size());
    return ESP_OK;
}

esp_err_t SendOk(httpd_req_t* req, const std::string& extra = "") {
    std::string json = std::string("{\"ok\":true") + (extra.empty() ? "" : "," + extra) + "}";
    return SendJson(req, "200 OK", json);
}

esp_err_t SendError(httpd_req_t* req, const char* message) {
    std::string json = std::string("{\"ok\":false,\"error\":\"") + message + "\"}";
    return SendJson(req, "400 Bad Request", json);
}

esp_err_t SendJpeg(httpd_req_t* req, const std::string& jpeg) {
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    size_t offset = 0;
    while (offset < jpeg.size()) {
        size_t n = std::min(kChunkSize, jpeg.size() - offset);
        int ret = httpd_resp_send_chunk(req, jpeg.data() + offset, static_cast<int>(n));
        if (ret != ESP_OK) {
            return ret;
        }
        offset += n;
    }
    return httpd_resp_send_chunk(req, nullptr, 0);
}

// ---------- handlers ----------

esp_err_t HandleRoot(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, kIndexHtml, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t HandleStatus(httpd_req_t* req) {
    auto& board = Board::GetInstance();
    auto status = board.GetDeviceStatusJson();
    cJSON* root = cJSON_Parse(status.c_str());
    if (root == nullptr) {
        root = cJSON_CreateObject();
    }
    cJSON* control = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "control", control);
    cJSON_AddNumberToObject(control, "volume", board.GetAudioCodec()->output_volume());
    if (auto backlight = board.GetBacklight()) {
        cJSON_AddNumberToObject(control, "brightness", backlight->brightness());
    }
    cJSON_AddNumberToObject(control, "uptime_seconds",
                            static_cast<double>(esp_timer_get_time() / 1000000));
    char* printed = cJSON_PrintUnformatted(root);
    std::string json = printed != nullptr ? printed : "{\"ok\":false}";
    cJSON_free(printed);
    cJSON_Delete(root);
    return SendJson(req, "200 OK", json);
}

esp_err_t HandleChat(httpd_req_t* req) {
    cJSON* root = ParseBodyJson(req);
    if (root == nullptr) {
        return SendError(req, "invalid json");
    }
    cJSON* text = cJSON_GetObjectItem(root, "text");
    if (!cJSON_IsString(text) || text->valuestring == nullptr || text->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return SendError(req, "text is required");
    }
    std::string query = text->valuestring;
    cJSON_Delete(root);
    if (query.size() > 512) {
        query.resize(512);
    }
    ESP_LOGI(TAG, "Text chat (%d chars): %s", static_cast<int>(query.size()), query.c_str());
    Application::GetInstance().SendTextMessage(query);
    return SendOk(req);
}

esp_err_t HandlePhoto(httpd_req_t* req) {
    auto camera = Board::GetInstance().GetCamera();
    if (camera == nullptr) {
        return SendError(req, "camera not available");
    }
    std::string jpeg;
    if (!camera->CaptureToJpeg(jpeg, 60)) {
        return SendError(req, "capture failed");
    }
    return SendJpeg(req, jpeg);
}

esp_err_t HandleScreen(httpd_req_t* req) {
#ifdef HAVE_LVGL
    auto display = Board::GetInstance().GetDisplay();
    if (display == nullptr || !display->SupportsGuiOperations()) {
        return SendError(req, "screen snapshot not supported");
    }
    std::string jpeg;
    if (!display->SnapshotToJpeg(jpeg, 70)) {
        return SendError(req, "snapshot failed");
    }
    return SendJpeg(req, jpeg);
#else
    return SendError(req, "screen snapshot not supported");
#endif
}

esp_err_t HandleVolume(httpd_req_t* req) {
    cJSON* root = ParseBodyJson(req);
    if (root == nullptr) {
        return SendError(req, "invalid json");
    }
    cJSON* value = cJSON_GetObjectItem(root, "volume");
    if (!cJSON_IsNumber(value)) {
        cJSON_Delete(root);
        return SendError(req, "volume must be a number");
    }
    int volume = std::clamp(value->valueint, 0, 100);
    cJSON_Delete(root);
    Board::GetInstance().GetAudioCodec()->SetOutputVolume(volume);
    return SendOk(req, "\"volume\":" + std::to_string(volume));
}

esp_err_t HandleBrightness(httpd_req_t* req) {
    auto backlight = Board::GetInstance().GetBacklight();
    if (backlight == nullptr) {
        return SendError(req, "backlight not available");
    }
    cJSON* root = ParseBodyJson(req);
    if (root == nullptr) {
        return SendError(req, "invalid json");
    }
    cJSON* value = cJSON_GetObjectItem(root, "brightness");
    if (!cJSON_IsNumber(value)) {
        cJSON_Delete(root);
        return SendError(req, "brightness must be a number");
    }
    int brightness = std::clamp(value->valueint, 0, 100);
    cJSON_Delete(root);
    backlight->SetBrightness(static_cast<uint8_t>(brightness), true);
    return SendOk(req, "\"brightness\":" + std::to_string(brightness));
}

esp_err_t HandleTheme(httpd_req_t* req) {
    cJSON* root = ParseBodyJson(req);
    if (root == nullptr) {
        return SendError(req, "invalid json");
    }
    cJSON* theme_json = cJSON_GetObjectItem(root, "theme");
    if (!cJSON_IsString(theme_json)) {
        cJSON_Delete(root);
        return SendError(req, "theme must be a string");
    }
    std::string theme_name = theme_json->valuestring;
    cJSON_Delete(root);
#ifdef HAVE_LVGL
    auto display = Board::GetInstance().GetDisplay();
    if (display == nullptr || display->GetTheme() == nullptr) {
        return SendError(req, "theme not supported");
    }
    auto theme = LvglThemeManager::GetInstance().GetTheme(theme_name);
    if (theme == nullptr) {
        return SendError(req, "unknown theme");
    }
    display->SetTheme(theme);
    return SendOk(req);
#else
    return SendError(req, "theme not supported");
#endif
}

esp_err_t HandleMotor(httpd_req_t* req) {
    auto* motor = MotorController::GetInstance();
    if (motor == nullptr) {
        return SendError(req, "motor not available");
    }
    cJSON* root = ParseBodyJson(req);
    if (root == nullptr) {
        return SendError(req, "invalid json");
    }
    cJSON* action_json = cJSON_GetObjectItem(root, "action");
    if (!cJSON_IsString(action_json) || action_json->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return SendError(req, "action must be forward/backward/left/right/stop");
    }
    std::string action = action_json->valuestring;
    int speed = 100;
    int duration_ms = 1500;
    cJSON* speed_json = cJSON_GetObjectItem(root, "speed");
    if (cJSON_IsNumber(speed_json)) {
        speed = std::clamp(speed_json->valueint, 30, 100);
    }
    cJSON* duration_json = cJSON_GetObjectItem(root, "duration_ms");
    if (cJSON_IsNumber(duration_json)) {
        duration_ms = std::clamp(duration_json->valueint, 100, 5000);
    }
    cJSON_Delete(root);

    std::string reply;
    if (!motor->Drive(action, speed, duration_ms, reply)) {
        return SendError(req, reply.c_str());
    }
    ESP_LOGI(TAG, "Motor API: %s", reply.c_str());
    return SendOk(req);
}

esp_err_t HandleServo(httpd_req_t* req) {
    auto* motor = MotorController::GetInstance();
    if (motor == nullptr) {
        return SendError(req, "motor not available");
    }
    cJSON* root = ParseBodyJson(req);
    if (root == nullptr) {
        return SendError(req, "invalid json");
    }
    int angle = 90;
    cJSON* angle_json = cJSON_GetObjectItem(root, "angle");
    if (cJSON_IsNumber(angle_json)) {
        angle = std::clamp(angle_json->valueint, 0, 180);
    }
    int channel = 4;
    cJSON* channel_json = cJSON_GetObjectItem(root, "channel");
    if (cJSON_IsNumber(channel_json)) {
        channel = std::clamp(channel_json->valueint, 0, 15);
    }
    cJSON_Delete(root);

    motor->SetServoAngle(channel, angle);
    ESP_LOGI(TAG, "Servo API: channel %d -> %d deg", channel, angle);
    return SendOk(req);
}

esp_err_t HandleReboot(httpd_req_t* req) {
    auto& app = Application::GetInstance();
    app.Schedule([&app]() {
        vTaskDelay(pdMS_TO_TICKS(500));
        app.Reboot();
    });
    return SendOk(req);
}

#ifdef CONFIG_BOARD_TYPE_BREAD_COMPACT_WIFI_S3CAM_DOG
esp_err_t HandleDog(httpd_req_t* req) {
    cJSON* root = ParseBodyJson(req);
    if (root == nullptr) return SendError(req, "invalid json");
    cJSON* action_json = cJSON_GetObjectItem(root, "action");
    if (!cJSON_IsString(action_json)) { cJSON_Delete(root); return SendError(req, "missing action"); }
    std::string action = action_json->valuestring;
    int steps = 4;
    cJSON* s = cJSON_GetObjectItem(root, "steps");
    if (cJSON_IsNumber(s)) steps = std::clamp(s->valueint, 1, 10);
    cJSON_Delete(root);

    int action_type = 0, speed = 1000, direction = 0, height = 45;
    if (action == "walk_forward") { action_type = DOG_WALK; direction = 1; }
    else if (action == "walk_backward") { action_type = DOG_WALK; direction = -1; }
    else if (action == "turn_left") { action_type = DOG_TURN; direction = 1; speed = 2000; }
    else if (action == "turn_right") { action_type = DOG_TURN; direction = -1; speed = 2000; }
    else if (action == "sit") { action_type = DOG_SIT; speed = 1500; }
    else if (action == "stand") { action_type = DOG_STAND; speed = 1500; }
    else if (action == "stretch") { action_type = DOG_STRETCH; speed = 2000; }
    else if (action == "shake") { action_type = DOG_SHAKE; speed = 1000; }
    else if (action == "lift_lf") { action_type = DOG_LIFT_LF; }
    else if (action == "lift_lr") { action_type = DOG_LIFT_LR; }
    else if (action == "lift_rf") { action_type = DOG_LIFT_RF; }
    else if (action == "lift_rr") { action_type = DOG_LIFT_RR; }
    else if (action == "home") { action_type = DOG_HOME; }
    else if (action == "sleep") { action_type = DOG_SLEEP; speed = 1500; }
    else return SendError(req, "unknown action");

    if (!QueueDogAction(action_type, steps, speed, direction, height)) {
        return SendError(req, "dog controller not ready");
    }
    ESP_LOGI(TAG, "Dog API: %s", action.c_str());
    return SendOk(req);
}

esp_err_t HandleDogTrimGet(httpd_req_t* req) {
    Settings settings("dog_trims", false);
    int lf = settings.GetInt("left_front_leg", 0);
    int lr = settings.GetInt("left_rear_leg", 0);
    int rf = settings.GetInt("right_front_leg", 0);
    int rr = settings.GetInt("right_rear_leg", 0);
    cJSON* json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "ok", true);
    cJSON_AddNumberToObject(json, "left_front_leg", lf);
    cJSON_AddNumberToObject(json, "left_rear_leg", lr);
    cJSON_AddNumberToObject(json, "right_front_leg", rf);
    cJSON_AddNumberToObject(json, "right_rear_leg", rr);
    char* str = cJSON_PrintUnformatted(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, str);
    cJSON_free(str);
    cJSON_Delete(json);
    return ESP_OK;
}

esp_err_t HandleDogTrimSet(httpd_req_t* req) {
    cJSON* root = ParseBodyJson(req);
    if (root == nullptr) return SendError(req, "invalid json");
    cJSON* servo_json = cJSON_GetObjectItem(root, "servo");
    cJSON* trim_json = cJSON_GetObjectItem(root, "trim");
    if (!cJSON_IsString(servo_json) || !cJSON_IsNumber(trim_json)) {
        cJSON_Delete(root);
        return SendError(req, "missing servo or trim");
    }
    std::string servo = servo_json->valuestring;
    int trim = std::clamp(trim_json->valueint, -50, 50);
    cJSON_Delete(root);

    Settings settings("dog_trims", true);
    if (servo == "left_front_leg") { settings.SetInt("left_front_leg", trim); }
    else if (servo == "left_rear_leg") { settings.SetInt("left_rear_leg", trim); }
    else if (servo == "right_front_leg") { settings.SetInt("right_front_leg", trim); }
    else if (servo == "right_rear_leg") { settings.SetInt("right_rear_leg", trim); }
    else return SendError(req, "unknown servo");

    // 重新应用所有微调（通过 HOME 动作让新微调值生效）
    if (IsDogBusy() == false) {
        QueueDogAction(DOG_HOME, 1, 1000, 0, 0);
    }

    ESP_LOGI(TAG, "Dog trim set: %s = %d", servo.c_str(), trim);
    return SendOk(req);
}
#endif

}  // namespace

bool LocalControlServer::Start(int port) {
    if (server_ != nullptr) {
        return true;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.ctrl_port = 32770;
    config.max_open_sockets = 6;
    config.max_uri_handlers = 18;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 15;

    if (httpd_start(&server_, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start local control server on port %d", port);
        server_ = nullptr;
        return false;
    }

    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = HandleRoot, .user_ctx = nullptr},
        {.uri = "/api/status", .method = HTTP_GET, .handler = HandleStatus, .user_ctx = nullptr},
        {.uri = "/api/chat", .method = HTTP_POST, .handler = HandleChat, .user_ctx = nullptr},
        {.uri = "/api/photo.jpg", .method = HTTP_GET, .handler = HandlePhoto, .user_ctx = nullptr},
        {.uri = "/api/screen.jpg", .method = HTTP_GET, .handler = HandleScreen, .user_ctx = nullptr},
        {.uri = "/api/volume", .method = HTTP_POST, .handler = HandleVolume, .user_ctx = nullptr},
        {.uri = "/api/brightness", .method = HTTP_POST, .handler = HandleBrightness, .user_ctx = nullptr},
        {.uri = "/api/theme", .method = HTTP_POST, .handler = HandleTheme, .user_ctx = nullptr},
        {.uri = "/api/motor", .method = HTTP_POST, .handler = HandleMotor, .user_ctx = nullptr},
        {.uri = "/api/servo", .method = HTTP_POST, .handler = HandleServo, .user_ctx = nullptr},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = HandleReboot, .user_ctx = nullptr},
#ifdef CONFIG_BOARD_TYPE_BREAD_COMPACT_WIFI_S3CAM_DOG
        {.uri = "/api/dog", .method = HTTP_POST, .handler = HandleDog, .user_ctx = nullptr},
        {.uri = "/api/dog/trim", .method = HTTP_GET, .handler = HandleDogTrimGet, .user_ctx = nullptr},
        {.uri = "/api/dog/trim", .method = HTTP_POST, .handler = HandleDogTrimSet, .user_ctx = nullptr},
#endif
    };
    for (const auto& uri : uris) {
        httpd_register_uri_handler(server_, &uri);
    }

    ESP_LOGI(TAG, "Local control server started on port %d", port);
    return true;
}

void LocalControlServer::Stop() {
    if (server_ != nullptr) {
        httpd_stop(server_);
        server_ = nullptr;
    }
}
