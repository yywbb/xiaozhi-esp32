#ifndef LOCAL_CONTROL_SERVER_H_
#define LOCAL_CONTROL_SERVER_H_

#include <esp_http_server.h>

// LAN HTTP control panel for mobile browsers.
// Serves a mobile-friendly page at http://<device-ip>:8080/ plus JSON APIs:
//   GET  /api/status        device status JSON
//   POST /api/chat          {"text":"..."}  start a text conversation
//   GET  /api/photo.jpg     capture a camera frame
//   GET  /api/screen.jpg    snapshot the screen
//   POST /api/volume        {"volume":0-100}
//   POST /api/brightness    {"brightness":0-100}
//   POST /api/theme         {"theme":"light"|"dark"}
//   POST /api/motor         {"action":"forward"|"backward"|"left"|"right"|"stop",
//                            "speed":30-100,"duration_ms":100-5000}
//   POST /api/reboot
class LocalControlServer {
public:
    bool Start(int port = 8080);
    void Stop();

private:
    httpd_handle_t server_ = nullptr;
};

#endif  // LOCAL_CONTROL_SERVER_H_
