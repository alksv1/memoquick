#pragma once
#include "core.hpp"
#include <mmsystem.h>
#include <array>

namespace mq {
// Owns the microphone buffers; polling and all lifecycle calls run on the UI thread.
class AudioRecorder {
public:
    ~AudioRecorder();
    void start();
    bool poll();
    std::string finish();
    void cancel();
    bool active() const { return device_ != nullptr; }
    unsigned seconds() const { return static_cast<unsigned>(samples_.size() / 32000); }
private:
    HWAVEIN device_ = nullptr;
    std::array<WAVEHDR, 4> headers_{};
    std::array<std::array<char, 8000>, 4> buffers_{};
    std::string samples_;
    size_t next_ = 0;
};
std::string waveFile(const std::string& samples);
} // namespace mq
