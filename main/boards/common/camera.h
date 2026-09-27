#ifndef CAMERA_H
#define CAMERA_H

#include <expected>
#include <string>

class Camera {
public:
    virtual ~Camera() = default;

    virtual void SetExplainUrl(const std::string& url, const std::string& token) = 0;
    virtual bool Capture() = 0;
    virtual bool SetHMirror(bool enabled) = 0;
    virtual bool SetVFlip(bool enabled) = 0;
    virtual bool SetSwapBytes(bool enabled) { return false; }  // Optional, default no-op
    virtual std::expected<std::string, std::string> Explain(const std::string& question) = 0;
    // Capture a frame synchronously and encode it as JPEG into jpeg_data. Optional.
    virtual bool CaptureToJpeg(std::string& jpeg_data, int quality = 80) {
        (void)jpeg_data;
        (void)quality;
        return false;
    }
};

#endif  // CAMERA_H
