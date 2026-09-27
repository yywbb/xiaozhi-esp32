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

// NOTE: display.h defines HAVE_LVGL when LVGL support is compiled in, so these
// must be included unconditionally (same pattern as mcp_server.cc); wrapping
// them in #ifdef HAVE_LVGL would leave the macro undefined for the handlers
// below, disabling screen snapshot and theme switching silently.
#include "display.h"
#include "lvgl_theme.h"
#include "motor_controller.h"

#define TAG "LocalCtrl"

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
        return SendError(req, "action must be forward/backward/stop");
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

esp_err_t HandleReboot(httpd_req_t* req) {
    auto& app = Application::GetInstance();
    app.Schedule([&app]() {
        vTaskDelay(pdMS_TO_TICKS(500));
        app.Reboot();
    });
    return SendOk(req);
}

}  // namespace

bool LocalControlServer::Start(int port) {
    if (server_ != nullptr) {
        return true;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.ctrl_port = 32770;
    config.max_open_sockets = 6;
    config.max_uri_handlers = 12;
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
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = HandleReboot, .user_ctx = nullptr},
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
