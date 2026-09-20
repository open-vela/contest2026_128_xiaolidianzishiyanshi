#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace xiaozhi {

class AudioDevice {
 public:
  using EncodedCallback =
      std::function<void(std::vector<uint8_t>, uint32_t timestamp)>;
  using ErrorCallback = std::function<void(const std::string &)>;

  virtual ~AudioDevice() = default;
  virtual bool Start(EncodedCallback encoded, ErrorCallback error) = 0;
  virtual void Stop() = 0;
  virtual void SetCaptureEnabled(bool enabled) = 0;
  virtual void SetOutputSampleRate(int sample_rate) = 0;
  virtual void QueuePlayback(std::vector<uint8_t> opus) = 0;
  virtual void ClearPlayback() = 0;
};

std::unique_ptr<AudioDevice> CreateNuttxAudioDevice(
    const std::string &capture_path, const std::string &playback_path);

}  // namespace xiaozhi
