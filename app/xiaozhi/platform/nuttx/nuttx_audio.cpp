#include "xiaozhi/audio.h"

#include <nuttx/audio/audio.h>
#include <nuttx/config.h>
#include <opus.h>

#include <fcntl.h>
#include <mqueue.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "bsp_test_sound.h"

namespace xiaozhi {
namespace {

#ifdef CONFIG_CONTEST2026_128_XIAOZHI_AUDIO_DIAGNOSTICS
#define xiaozhi_alog(level, format, ...)                                       \
  syslog(level, "xiaozhi audio: " format, ##__VA_ARGS__)
#define xiaozhi_ainfo(format, ...) xiaozhi_alog(LOG_INFO, format, ##__VA_ARGS__)
#define xiaozhi_awarn(format, ...)                                             \
  xiaozhi_alog(LOG_WARNING, format, ##__VA_ARGS__)
#define xiaozhi_aerr(format, ...) xiaozhi_alog(LOG_ERR, format, ##__VA_ARGS__)
#else
#define xiaozhi_alog(level, format, ...)                                       \
  do {                                                                         \
  } while (0)
#define xiaozhi_ainfo(format, ...)                                             \
  do {                                                                         \
  } while (0)
#define xiaozhi_awarn(format, ...)                                             \
  do {                                                                         \
  } while (0)
#define xiaozhi_aerr(format, ...)                                              \
  do {                                                                         \
  } while (0)
#endif

constexpr int kCaptureSampleRate = 16000;
constexpr int kFrameDurationMs = 60;
constexpr int kCaptureSamples = kCaptureSampleRate * kFrameDurationMs / 1000;
constexpr int kBitsPerSample = 16;
constexpr int kChannels = 1;
constexpr size_t kCaptureSlotCount =
    CONFIG_CONTEST2026_128_XIAOZHI_CAPTURE_SLOT_COUNT;
constexpr int kFallbackBufferCount = 4;
constexpr int kFallbackBufferSize = kCaptureSamples * sizeof(int16_t);
constexpr size_t kLoopbackSamples = kCaptureSampleRate * 2;
constexpr size_t kTestSoundSamples = kCaptureSampleRate * 3 / 4;

int CreateAudioThread(pthread_t *thread, void *(*entry)(void *),
                      void *argument) {
  pthread_attr_t attributes;
  int result = pthread_attr_init(&attributes);
  if (result != 0) {
    return result;
  }

  result = pthread_attr_setstacksize(
      &attributes, CONFIG_CONTEST2026_128_XIAOZHI_AUDIO_STACKSIZE);
  if (result == 0) {
    result = pthread_create(thread, &attributes, entry, argument);
  }
  pthread_attr_destroy(&attributes);
  return result;
}

uint32_t MonotonicMilliseconds() {
  struct timespec now {};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint32_t>(now.tv_sec * 1000ULL + now.tv_nsec / 1000000);
}

int32_t SampleMagnitude(int16_t sample) {
  const int32_t value = sample;
  return value < 0 ? -value : value;
}

/* ESP32-P4 standard I2S transfers two physical slots per frame even when the
 * ES8311 ADC is mono.  The NuttX ES8311 lower half configures sample rate and
 * width but currently leaves both RX slots in each DMA buffer.  Select the
 * slot carrying the greater signal energy so Opus receives exactly one
 * 16-kHz sample per frame, matching the upstream XiaoZhi audio contract.
 */
size_t ExtractCaptureMono(const int16_t *input, size_t input_samples,
                          std::vector<int16_t> *output, size_t *selected_slot,
                          uint64_t *slot0_energy, uint64_t *slot1_energy) {
  output->clear();
  *selected_slot = 0;
  *slot0_energy = 0;
  *slot1_energy = 0;

  if (kCaptureSlotCount == 1) {
    output->assign(input, input + input_samples);
    for (size_t index = 0; index < input_samples; ++index) {
      *slot0_energy += SampleMagnitude(input[index]);
    }
    return input_samples;
  }

  const size_t frames = input_samples / kCaptureSlotCount;
  for (size_t index = 0; index < frames; ++index) {
    *slot0_energy += SampleMagnitude(input[index * kCaptureSlotCount]);
    *slot1_energy += SampleMagnitude(input[index * kCaptureSlotCount + 1]);
  }
  *selected_slot = *slot1_energy > *slot0_energy ? 1 : 0;
  output->reserve(frames);
  for (size_t index = 0; index < frames; ++index) {
    output->push_back(input[index * kCaptureSlotCount + *selected_slot]);
  }
  return frames;
}

void ExpandPlaybackSlots(const int16_t *mono, size_t samples,
                         std::vector<int16_t> *output) {
  output->clear();
  if (kCaptureSlotCount == 1) {
    output->assign(mono, mono + samples);
    return;
  }

  /* The ESP32-P4 I2S link clocks two physical slots per audio frame.  The
   * Opus decoder returns mono PCM, so duplicate each sample into both slots.
   * Sending packed mono directly would make a 60-ms packet occupy only
   * 30 ms on the wire and feed alternating samples to the codec channel.
   */

  output->reserve(samples * kCaptureSlotCount);
  for (size_t index = 0; index < samples; ++index) {
    for (size_t slot = 0; slot < kCaptureSlotCount; ++slot) {
      output->push_back(mono[index]);
    }
  }
}

struct AudioEndpoint {
  int fd{-1};
  mqd_t mq{static_cast<mqd_t>(-1)};
  std::string mq_name;
  std::vector<ap_buffer_s *> buffers;
#ifdef CONFIG_AUDIO_MULTI_SESSION
  void *session{nullptr};
#endif
};

struct RawPlayback {
  std::vector<int16_t> samples;
  AudioDevice::TestCallback callback;
  std::string success_message;
};

unsigned long SessionArgument(AudioEndpoint &endpoint) {
#ifdef CONFIG_AUDIO_MULTI_SESSION
  return reinterpret_cast<unsigned long>(endpoint.session);
#else
  (void)endpoint;
  return 0;
#endif
}

void SetDescriptorSession(audio_buf_desc_s &descriptor,
                          AudioEndpoint &endpoint) {
#ifdef CONFIG_AUDIO_MULTI_SESSION
  descriptor.session = endpoint.session;
#else
  (void)descriptor;
  (void)endpoint;
#endif
}

void SetCapabilitiesSession(audio_caps_desc_s &descriptor,
                            AudioEndpoint &endpoint) {
#ifdef CONFIG_AUDIO_MULTI_SESSION
  descriptor.session = endpoint.session;
#else
  (void)descriptor;
  (void)endpoint;
#endif
}

int ConfigureEndpoint(AudioEndpoint &endpoint, const std::string &path,
                      uint8_t type, int sample_rate, const char *queue_suffix) {
  xiaozhi_ainfo("open path=%s type=%u rate=%d\n", path.c_str(), type,
                sample_rate);
  endpoint.fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (endpoint.fd < 0) {
    return -errno;
  }
  xiaozhi_ainfo("open ok fd=%d\n", endpoint.fd);

#ifdef CONFIG_AUDIO_MULTI_SESSION
  if (ioctl(endpoint.fd, AUDIOIOC_RESERVE,
            reinterpret_cast<unsigned long>(&endpoint.session)) < 0) {
#else
  if (ioctl(endpoint.fd, AUDIOIOC_RESERVE, 0) < 0) {
#endif
    const int result = -errno;
    close(endpoint.fd);
    endpoint.fd = -1;
    return result;
  }
  xiaozhi_ainfo("reserve ok fd=%d\n", endpoint.fd);

  struct audio_caps_desc_s capabilities {};
  SetCapabilitiesSession(capabilities, endpoint);
  capabilities.caps.ac_len = sizeof(audio_caps_s);
  capabilities.caps.ac_type = type;
  capabilities.caps.ac_subtype = AUDIO_FMT_PCM;
  capabilities.caps.ac_channels = kChannels;
  capabilities.caps.ac_chmap = 1;
  capabilities.caps.ac_controls.hw[0] = sample_rate;
  capabilities.caps.ac_controls.b[3] = sample_rate >> 16;
  capabilities.caps.ac_controls.b[2] = kBitsPerSample;
  if (ioctl(endpoint.fd, AUDIOIOC_CONFIGURE,
            reinterpret_cast<unsigned long>(&capabilities)) < 0) {
    return -errno;
  }
  xiaozhi_ainfo("configure ok fd=%d\n", endpoint.fd);

  struct ap_buffer_info_s info {};
  if (ioctl(endpoint.fd, AUDIOIOC_GETBUFFERINFO,
            reinterpret_cast<unsigned long>(&info)) < 0 ||
      info.nbuffers == 0 || info.buffer_size == 0) {
    info.nbuffers = kFallbackBufferCount;
    info.buffer_size = kFallbackBufferSize;
  }
  xiaozhi_ainfo("buffers count=%u size=%u\n",
                static_cast<unsigned int>(info.nbuffers),
                static_cast<unsigned int>(info.buffer_size));

  char name[32];
  std::snprintf(
      name, sizeof(name), "/xz-%lx-%s",
      static_cast<unsigned long>(reinterpret_cast<uintptr_t>(&endpoint)),
      queue_suffix);
  endpoint.mq_name = name;
  struct mq_attr attributes {};
  attributes.mq_maxmsg = info.nbuffers + 4;
  attributes.mq_msgsize = sizeof(audio_msg_s);
  endpoint.mq = mq_open(endpoint.mq_name.c_str(), O_RDWR | O_CREAT | O_NONBLOCK,
                        0644, &attributes);
  if (endpoint.mq == static_cast<mqd_t>(-1)) {
    return -errno;
  }
  if (ioctl(endpoint.fd, AUDIOIOC_REGISTERMQ,
            static_cast<unsigned long>(endpoint.mq)) < 0) {
    return -errno;
  }
  xiaozhi_ainfo("message queue registered\n");

  endpoint.buffers.resize(info.nbuffers, nullptr);
  for (auto &buffer : endpoint.buffers) {
    struct audio_buf_desc_s descriptor {};
    SetDescriptorSession(descriptor, endpoint);
    descriptor.numbytes = info.buffer_size;
    descriptor.u.pbuffer = &buffer;
    const int result = ioctl(endpoint.fd, AUDIOIOC_ALLOCBUFFER,
                             reinterpret_cast<unsigned long>(&descriptor));
    if (result != sizeof(descriptor)) {
      return result < 0 ? -errno : -ENOMEM;
    }
  }
  xiaozhi_ainfo("allocated %zu buffers\n", endpoint.buffers.size());
  return 0;
}

int Enqueue(AudioEndpoint &endpoint, ap_buffer_s *buffer, bool capture) {
  struct audio_buf_desc_s descriptor {};
  SetDescriptorSession(descriptor, endpoint);
  buffer->curbyte = 0;
  buffer->flags = 0;
  if (capture) {
    buffer->nbytes = buffer->nmaxbytes;
  }
  descriptor.numbytes = buffer->nbytes;
  descriptor.u.buffer = buffer;
  if (ioctl(endpoint.fd, AUDIOIOC_ENQUEUEBUFFER,
            reinterpret_cast<unsigned long>(&descriptor)) < 0) {
    return -errno;
  }
  return 0;
}

void CloseEndpoint(AudioEndpoint &endpoint) {
  if (endpoint.fd < 0) {
    return;
  }
  xiaozhi_ainfo("closing fd=%d\n", endpoint.fd);
  ioctl(endpoint.fd, AUDIOIOC_STOP, SessionArgument(endpoint));
  if (endpoint.mq != static_cast<mqd_t>(-1)) {
    ioctl(endpoint.fd, AUDIOIOC_UNREGISTERMQ,
          static_cast<unsigned long>(endpoint.mq));
  }
  for (auto *buffer : endpoint.buffers) {
    if (buffer == nullptr) {
      continue;
    }
    struct audio_buf_desc_s descriptor {};
    SetDescriptorSession(descriptor, endpoint);
    descriptor.u.buffer = buffer;
    ioctl(endpoint.fd, AUDIOIOC_FREEBUFFER,
          reinterpret_cast<unsigned long>(&descriptor));
  }
  endpoint.buffers.clear();
  ioctl(endpoint.fd, AUDIOIOC_RELEASE, SessionArgument(endpoint));
  close(endpoint.fd);
  endpoint.fd = -1;
  if (endpoint.mq != static_cast<mqd_t>(-1)) {
    mq_close(endpoint.mq);
    mq_unlink(endpoint.mq_name.c_str());
    endpoint.mq = static_cast<mqd_t>(-1);
  }
  xiaozhi_ainfo("closed\n");
}

class NuttxAudioDevice final : public AudioDevice {
public:
  NuttxAudioDevice(std::string capture_path, std::string playback_path)
      : capture_path_(std::move(capture_path)),
        playback_path_(std::move(playback_path)) {}

  ~NuttxAudioDevice() override { Stop(); }

  bool Start(EncodedCallback encoded, ErrorCallback error) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (running_) {
        return true;
      }
      encoded_callback_ = std::move(encoded);
      error_callback_ = std::move(error);
      running_ = true;
    }
    if (CreateAudioThread(&capture_thread_, CaptureEntry, this) != 0) {
      std::lock_guard<std::mutex> lock(mutex_);
      running_ = false;
      return false;
    }
    pthread_setname_np(capture_thread_, "xz-capture");
    capture_started_ = true;
    if (CreateAudioThread(&playback_thread_, PlaybackEntry, this) != 0) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
      }
      pthread_join(capture_thread_, nullptr);
      capture_started_ = false;
      return false;
    }
    pthread_setname_np(playback_thread_, "xz-playback");
    playback_started_ = true;
    return true;
  }

  void Stop() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!running_ && !capture_started_ && !playback_started_) {
        return;
      }
      running_ = false;
      capture_enabled_ = false;
      playback_queue_.clear();
      playback_primed_ = false;
    }
    playback_wake_.notify_all();
    if (capture_started_) {
      pthread_join(capture_thread_, nullptr);
      capture_started_ = false;
    }
    if (playback_started_) {
      pthread_join(playback_thread_, nullptr);
      playback_started_ = false;
    }
  }

  void SetCaptureEnabled(bool enabled) override {
    std::lock_guard<std::mutex> lock(mutex_);
    capture_enabled_ = enabled;
  }

  void SetOutputSampleRate(int sample_rate) override {
    if (sample_rate != 8000 && sample_rate != 12000 && sample_rate != 16000 &&
        sample_rate != 24000 && sample_rate != 48000) {
      ReportError("unsupported Opus output sample rate");
      return;
    }

    /* Opus can decode every supported stream rate to 16 kHz.  Keep the
     * physical ES8311 link at the capture rate because its ADC and DAC share
     * one codec clock; independently switching playback to (for example)
     * 24 kHz would also change the microphone sampling clock.
     */
    xiaozhi_ainfo("server playback rate=%d, decoder/hardware rate=%d\n",
                  sample_rate, kCaptureSampleRate);
  }

  void QueuePlayback(std::vector<uint8_t> opus) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!running_) {
        return;
      }
      if (playback_queue_.size() >= 32) {
        playback_queue_.pop_front();
      }
      playback_queue_.push_back(std::move(opus));
    }
    playback_wake_.notify_one();
  }

  void ClearPlayback() override {
    std::lock_guard<std::mutex> lock(mutex_);
    playback_queue_.clear();
    playback_primed_ = false;
  }

  bool RunMicrophoneLoopback(TestCallback callback) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || capture_enabled_ || loopback_recording_) {
      return false;
    }
    loopback_samples_.clear();
    loopback_samples_.reserve(kLoopbackSamples);
    loopback_callback_ = std::move(callback);
    loopback_recording_ = true;
    return true;
  }

  bool PlayTestSound(TestCallback callback) override {
    RawPlayback playback;
    playback.samples.reserve(kTestSoundSamples);
    for (size_t index = 0; index < kTestSoundSamples; ++index) {
      /* Alternate two amplitudes to make the test sound easy to distinguish
       * from speech while keeping the entire source waveform in a header.
       */
      int16_t sample = kBspTestTone[index % kBspTestToneSamples];
      if ((index / (kCaptureSampleRate / 4)) & 1) {
        sample /= 2;
      }
      playback.samples.push_back(sample);
    }
    playback.callback = std::move(callback);
    playback.success_message = "测试音效已提交到喇叭";

    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!running_) {
        return false;
      }
      raw_playback_queue_.push_back(std::move(playback));
    }
    playback_wake_.notify_one();
    return true;
  }

private:
  static void *CaptureEntry(void *argument) {
    static_cast<NuttxAudioDevice *>(argument)->CaptureLoop();
    return nullptr;
  }

  static void *PlaybackEntry(void *argument) {
    static_cast<NuttxAudioDevice *>(argument)->PlaybackLoop();
    return nullptr;
  }

  bool Running() {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
  }

  void CaptureLoop() {
    AudioEndpoint capture;
    int opus_error = OPUS_OK;
    OpusEncoder *encoder = opus_encoder_create(
        kCaptureSampleRate, kChannels, OPUS_APPLICATION_VOIP, &opus_error);
    if (encoder == nullptr || opus_error != OPUS_OK) {
      ReportError("opus_encoder_create failed");
      return;
    }
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(24000));
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(5));

    const int setup = ConfigureEndpoint(
        capture, capture_path_, AUDIO_TYPE_INPUT, kCaptureSampleRate, "cap");
    if (setup < 0) {
      CloseEndpoint(capture);
      opus_encoder_destroy(encoder);
      ReportError("cannot configure capture device " + capture_path_);
      return;
    }
    for (auto *buffer : capture.buffers) {
      if (Enqueue(capture, buffer, true) < 0) {
        CloseEndpoint(capture);
        opus_encoder_destroy(encoder);
        ReportError("cannot enqueue capture buffer");
        return;
      }
    }
    if (ioctl(capture.fd, AUDIOIOC_START, SessionArgument(capture)) < 0) {
      CloseEndpoint(capture);
      opus_encoder_destroy(encoder);
      ReportError("cannot start capture device");
      return;
    }

    std::vector<int16_t> pending;
    pending.reserve(kCaptureSamples * 2);
    std::vector<int16_t> mono;
    std::vector<uint8_t> encoded(512);
#ifdef CONFIG_CONTEST2026_128_XIAOZHI_AUDIO_DIAGNOSTICS
    uint32_t meter_start = MonotonicMilliseconds();
    uint64_t meter_sum = 0;
    size_t meter_samples = 0;
    size_t meter_frames = 0;
    size_t meter_bytes = 0;
    size_t meter_raw_samples = 0;
    size_t meter_slot1_buffers = 0;
    uint64_t meter_slot0_energy = 0;
    uint64_t meter_slot1_energy = 0;
    int32_t meter_peak = 0;
#endif
    while (Running()) {
      struct audio_msg_s message {};
      unsigned int priority = 0;
      const ssize_t received =
          mq_receive(capture.mq, reinterpret_cast<char *>(&message),
                     sizeof(message), &priority);
      if (received < 0) {
        if (errno == EAGAIN) {
          usleep(5000);
          continue;
        }
        if (errno == EINTR) {
          continue;
        }
        break;
      }
      if (message.msg_id == AUDIO_MSG_STOP) {
        break;
      }
      if (message.msg_id != AUDIO_MSG_DEQUEUE || message.u.ptr == nullptr) {
        continue;
      }

      auto *buffer = static_cast<ap_buffer_s *>(message.u.ptr);
      bool enabled;
      bool loopback;
      EncodedCallback callback;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        enabled = capture_enabled_;
        loopback = loopback_recording_;
        callback = encoded_callback_;
      }
      if (enabled || loopback) {
        const auto *samples = reinterpret_cast<const int16_t *>(buffer->samp);
        const size_t raw_sample_count = buffer->nbytes / sizeof(int16_t);
        size_t selected_slot;
        uint64_t slot0_energy;
        uint64_t slot1_energy;
        const size_t sample_count =
            ExtractCaptureMono(samples, raw_sample_count, &mono, &selected_slot,
                               &slot0_energy, &slot1_energy);

        if (loopback) {
          bool playback_ready = false;
          {
            std::lock_guard<std::mutex> lock(mutex_);
            if (loopback_recording_) {
              const size_t remaining =
                  kLoopbackSamples - loopback_samples_.size();
              const size_t copy_count =
                  sample_count < remaining ? sample_count : remaining;
              loopback_samples_.insert(loopback_samples_.end(), mono.begin(),
                                       mono.begin() + copy_count);
              if (loopback_samples_.size() == kLoopbackSamples) {
                RawPlayback playback;
                playback.samples = std::move(loopback_samples_);
                playback.callback = std::move(loopback_callback_);
                playback.success_message = "已录制 2 秒并提交喇叭回放";
                raw_playback_queue_.push_back(std::move(playback));
                loopback_recording_ = false;
                playback_ready = true;
              }
            }
          }
          if (playback_ready) {
            playback_wake_.notify_one();
          }
        }
#ifdef CONFIG_CONTEST2026_128_XIAOZHI_AUDIO_DIAGNOSTICS
        for (size_t index = 0; index < sample_count; ++index) {
          const int32_t magnitude = SampleMagnitude(mono[index]);
          if (magnitude > meter_peak) {
            meter_peak = magnitude;
          }
          meter_sum += magnitude;
        }
        meter_raw_samples += raw_sample_count;
        meter_samples += sample_count;
        meter_slot0_energy += slot0_energy;
        meter_slot1_energy += slot1_energy;
        if (selected_slot == 1) {
          ++meter_slot1_buffers;
        }
#endif
        if (enabled) {
          pending.insert(pending.end(), mono.begin(), mono.end());
        }
        while (enabled && pending.size() >= kCaptureSamples) {
          const int bytes =
              opus_encode(encoder, pending.data(), kCaptureSamples,
                          encoded.data(), encoded.size());
          if (bytes > 0 && callback) {
#ifdef CONFIG_CONTEST2026_128_XIAOZHI_AUDIO_DIAGNOSTICS
            ++meter_frames;
            meter_bytes += bytes;
#endif
            callback(
                std::vector<uint8_t>(encoded.begin(), encoded.begin() + bytes),
                MonotonicMilliseconds());
          }
          pending.erase(pending.begin(), pending.begin() + kCaptureSamples);
        }
      } else if (!loopback) {
        pending.clear();
      }
#ifdef CONFIG_CONTEST2026_128_XIAOZHI_AUDIO_DIAGNOSTICS
      const uint32_t meter_now = MonotonicMilliseconds();
      if (enabled && meter_now - meter_start >= 1000) {
        const unsigned long long mean =
            meter_samples == 0 ? 0 : meter_sum / meter_samples;
        xiaozhi_ainfo("raw_samples=%zu mono_samples=%zu peak=%ld mean=%llu "
                      "slot_energy=%llu/%llu slot1_buffers=%zu "
                      "opus_frames=%zu opus_bytes=%zu\n",
                      meter_raw_samples, meter_samples,
                      static_cast<long>(meter_peak), mean,
                      static_cast<unsigned long long>(meter_slot0_energy),
                      static_cast<unsigned long long>(meter_slot1_energy),
                      meter_slot1_buffers, meter_frames, meter_bytes);
        meter_start = meter_now;
        meter_sum = 0;
        meter_samples = 0;
        meter_frames = 0;
        meter_bytes = 0;
        meter_raw_samples = 0;
        meter_slot1_buffers = 0;
        meter_slot0_energy = 0;
        meter_slot1_energy = 0;
        meter_peak = 0;
      }
#endif
      if (Enqueue(capture, buffer, true) < 0) {
        break;
      }
    }

    CloseEndpoint(capture);
    opus_encoder_destroy(encoder);
  }

  void PlaybackLoop() {
    AudioEndpoint playback;
    int configured_rate = 0;
    OpusDecoder *decoder = nullptr;
    std::deque<ap_buffer_s *> free_buffers;
    std::vector<int16_t> decoded(5760);
    std::vector<int16_t> playback_slots;
    bool endpoint_ready = false;
#ifdef CONFIG_CONTEST2026_128_XIAOZHI_AUDIO_DIAGNOSTICS
    uint32_t playback_meter_start = MonotonicMilliseconds();
    size_t playback_meter_packets = 0;
    size_t playback_meter_samples = 0;
    size_t playback_meter_bytes = 0;
    int32_t playback_meter_peak = 0;
#endif

    while (Running()) {
      std::vector<uint8_t> packet;
      RawPlayback raw_playback;
      bool has_raw_playback = false;
      int wanted_rate;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        playback_wake_.wait(
            lock, [this]() {
              return !running_ || !raw_playback_queue_.empty() ||
                     !playback_queue_.empty();
            });
        if (!running_) {
          break;
        }

        if (!raw_playback_queue_.empty()) {
          raw_playback = std::move(raw_playback_queue_.front());
          raw_playback_queue_.pop_front();
          has_raw_playback = true;
        }

        /* Prime approximately 120 ms before the first DMA submission.  Two
         * 60-ms Opus packets give the ES8311/I2S queues enough headroom for
         * normal WebSocket and scheduler jitter.  Do not strand a short
         * response: when only one packet arrives, begin after 80 ms.
         */

        if (!has_raw_playback && !playback_primed_ &&
            playback_queue_.size() < 2) {
          playback_wake_.wait_for(
              lock, std::chrono::milliseconds(80),
              [this]() { return !running_ || playback_queue_.size() >= 2; });
          if (!running_) {
            break;
          }
        }
        if (!has_raw_playback && !playback_primed_) {
          xiaozhi_ainfo("playback primed packets=%zu\n",
                        playback_queue_.size());
          playback_primed_ = true;
        }
        wanted_rate = has_raw_playback ? kCaptureSampleRate : output_sample_rate_;
        if (!has_raw_playback && !playback_queue_.empty()) {
          packet = std::move(playback_queue_.front());
          playback_queue_.pop_front();
        }
      }

      if (!endpoint_ready || wanted_rate != configured_rate) {
        if (endpoint_ready) {
          CloseEndpoint(playback);
          free_buffers.clear();
          endpoint_ready = false;
        }
        if (decoder != nullptr) {
          opus_decoder_destroy(decoder);
          decoder = nullptr;
        }
        int opus_error = OPUS_OK;
        decoder = opus_decoder_create(wanted_rate, kChannels, &opus_error);
        if (decoder == nullptr || opus_error != OPUS_OK) {
          if (has_raw_playback && raw_playback.callback) {
            raw_playback.callback(false, "无法初始化喇叭解码器");
          }
          ReportError("opus_decoder_create failed");
          break;
        }
        if (ConfigureEndpoint(playback, playback_path_, AUDIO_TYPE_OUTPUT,
                              wanted_rate, "play") < 0) {
          CloseEndpoint(playback);
          if (has_raw_playback && raw_playback.callback) {
            raw_playback.callback(false, "无法配置喇叭设备");
          }
          ReportError("cannot configure playback device " + playback_path_);
          break;
        }
        for (auto *buffer : playback.buffers) {
          free_buffers.push_back(buffer);
        }
        if (ioctl(playback.fd, AUDIOIOC_START, SessionArgument(playback)) < 0) {
          if (has_raw_playback && raw_playback.callback) {
            raw_playback.callback(false, "无法启动喇叭设备");
          }
          ReportError("cannot start playback device");
          break;
        }
        configured_rate = wanted_rate;
        endpoint_ready = true;
      }

      struct audio_msg_s message {};
      unsigned int priority = 0;
      while (mq_receive(playback.mq, reinterpret_cast<char *>(&message),
                        sizeof(message), &priority) == sizeof(message)) {
        if (message.msg_id == AUDIO_MSG_DEQUEUE && message.u.ptr != nullptr) {
          free_buffers.push_back(static_cast<ap_buffer_s *>(message.u.ptr));
        }
      }

      if (!has_raw_playback && packet.empty()) {
        continue;
      }
      int samples;
      if (has_raw_playback) {
        samples = static_cast<int>(raw_playback.samples.size());
        ExpandPlaybackSlots(raw_playback.samples.data(),
                            raw_playback.samples.size(), &playback_slots);
      } else {
        samples = opus_decode(decoder, packet.data(), packet.size(),
                              decoded.data(), decoded.size(), 0);
        if (samples < 0) {
          ReportError("invalid Opus playback packet");
          continue;
        }
        ExpandPlaybackSlots(decoded.data(), static_cast<size_t>(samples),
                            &playback_slots);
      }
#ifdef CONFIG_CONTEST2026_128_XIAOZHI_AUDIO_DIAGNOSTICS
      ++playback_meter_packets;
      playback_meter_samples += samples;
      playback_meter_bytes += playback_slots.size() * sizeof(int16_t);
      const int16_t *meter_source = has_raw_playback
                                        ? raw_playback.samples.data()
                                        : decoded.data();
      for (int index = 0; index < samples; ++index) {
        const int32_t magnitude = SampleMagnitude(meter_source[index]);
        if (magnitude > playback_meter_peak) {
          playback_meter_peak = magnitude;
        }
      }
      const uint32_t playback_meter_now = MonotonicMilliseconds();
      if (playback_meter_now - playback_meter_start >= 1000) {
        xiaozhi_ainfo("playback packets=%zu mono_samples=%zu peak=%ld "
                      "i2s_bytes=%zu\n",
                      playback_meter_packets, playback_meter_samples,
                      static_cast<long>(playback_meter_peak),
                      playback_meter_bytes);
        playback_meter_start = playback_meter_now;
        playback_meter_packets = 0;
        playback_meter_samples = 0;
        playback_meter_bytes = 0;
        playback_meter_peak = 0;
      }
#endif

      const uint8_t *source =
          reinterpret_cast<const uint8_t *>(playback_slots.data());
      size_t remaining = playback_slots.size() * sizeof(int16_t);
      bool playback_ok = true;
      while (remaining > 0 && Running()) {
        if (free_buffers.empty()) {
          usleep(5000);
          while (mq_receive(playback.mq, reinterpret_cast<char *>(&message),
                            sizeof(message), &priority) == sizeof(message)) {
            if (message.msg_id == AUDIO_MSG_DEQUEUE &&
                message.u.ptr != nullptr) {
              free_buffers.push_back(static_cast<ap_buffer_s *>(message.u.ptr));
            }
          }
          continue;
        }
        ap_buffer_s *buffer = free_buffers.front();
        free_buffers.pop_front();
        const size_t bytes =
            remaining < buffer->nmaxbytes ? remaining : buffer->nmaxbytes;
        std::memcpy(buffer->samp, source, bytes);
        buffer->nbytes = bytes;
        buffer->curbyte = 0;
        if (Enqueue(playback, buffer, false) < 0) {
          ReportError("cannot enqueue playback buffer");
          playback_ok = false;
          remaining = 0;
          break;
        }
        source += bytes;
        remaining -= bytes;
      }
      if (has_raw_playback && raw_playback.callback) {
        const bool success = playback_ok && remaining == 0;
        raw_playback.callback(success,
                              success ? raw_playback.success_message
                                      : "喇叭写入失败或测试被中止");
      }
    }

    if (endpoint_ready) {
      CloseEndpoint(playback);
    }
    if (decoder != nullptr) {
      opus_decoder_destroy(decoder);
    }
  }

  void ReportError(const std::string &message) {
    ErrorCallback callback;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      callback = error_callback_;
    }
    if (callback) {
      callback(message);
    } else {
      std::fprintf(stderr, "xiaozhi audio: %s\n", message.c_str());
    }
  }

  std::string capture_path_;
  std::string playback_path_;
  std::mutex mutex_;
  std::condition_variable playback_wake_;
  bool running_{false};
  bool capture_enabled_{false};
  bool capture_started_{false};
  bool playback_started_{false};
  bool playback_primed_{false};
  int output_sample_rate_{kCaptureSampleRate};
  pthread_t capture_thread_{};
  pthread_t playback_thread_{};
  std::deque<std::vector<uint8_t>> playback_queue_;
  std::deque<RawPlayback> raw_playback_queue_;
  std::vector<int16_t> loopback_samples_;
  TestCallback loopback_callback_;
  bool loopback_recording_{false};
  EncodedCallback encoded_callback_;
  ErrorCallback error_callback_;
};

} // namespace

std::unique_ptr<AudioDevice>
CreateNuttxAudioDevice(const std::string &capture_path,
                       const std::string &playback_path) {
  return std::make_unique<NuttxAudioDevice>(capture_path, playback_path);
}

} // namespace xiaozhi
