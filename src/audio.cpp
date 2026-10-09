#include "audio.hpp"
#include <algorithm>

namespace mq {
std::string waveFile(const std::string& samples) {
    if (samples.empty() || samples.size() > AttachmentSizeMax - 44 || samples.size() % 2)
        throw Error("录音为空或长度异常。", ErrorKind::Validation);
    std::string result;
    const auto number = [&](size_t value, int count) {
        for (int i = 0; i < count; ++i) { result += static_cast<char>(value & 255); value >>= 8; }
    };
    result += "RIFF"; number(samples.size() + 36, 4); result += "WAVEfmt "; number(16, 4);
    number(1, 2); number(1, 2); number(16000, 4); number(32000, 4); number(2, 2); number(16, 2);
    result += "data"; number(samples.size(), 4); result += samples;
    return result;
}
AudioRecorder::~AudioRecorder() { cancel(); }
void AudioRecorder::start() {
    if (active()) return;
    samples_.clear(); next_ = 0; headers_ = {};
    WAVEFORMATEX format{WAVE_FORMAT_PCM,1,16000,32000,2,16,0};
    if (waveInOpen(&device_, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        device_ = nullptr;
        throw Error("无法打开麦克风，请检查设备及 Windows 麦克风权限。", ErrorKind::Storage);
    }
    for (size_t i = 0; i < headers_.size(); ++i) {
        auto& header = headers_[i];
        header.lpData = buffers_[i].data(); header.dwBufferLength = static_cast<DWORD>(buffers_[i].size());
        if (waveInPrepareHeader(device_, &header, sizeof(header)) != MMSYSERR_NOERROR
            || waveInAddBuffer(device_, &header, sizeof(header)) != MMSYSERR_NOERROR) {
            cancel(); throw Error("无法准备录音。", ErrorKind::Storage);
        }
    }
    if (waveInStart(device_) != MMSYSERR_NOERROR) { cancel(); throw Error("无法开始录音。", ErrorKind::Storage); }
}
bool AudioRecorder::poll() {
    if (!active()) return false;
    constexpr size_t limit = 32000 * 300;
    for (size_t i = 0; i < headers_.size(); ++i) {
        auto& header = headers_[next_];
        if (!(header.dwFlags & WHDR_DONE)) break;
        samples_.append(header.lpData, std::min<size_t>(header.dwBytesRecorded, limit - samples_.size()));
        if (samples_.size() >= limit) return true;
        header.dwBytesRecorded = 0;
        if (waveInAddBuffer(device_, &header, sizeof(header)) != MMSYSERR_NOERROR)
            throw Error("录音设备已断开，请停止并保留已有录音。", ErrorKind::Storage);
        next_ = (next_ + 1) % headers_.size();
    }
    return false;
}
std::string AudioRecorder::finish() {
    if (!active()) throw Error("录音尚未开始。", ErrorKind::Validation);
    waveInStop(device_); waveInReset(device_);
    for (size_t i = 0; i < headers_.size(); ++i) {
        const auto& header = headers_[(next_ + i) % headers_.size()];
        if (header.dwFlags & WHDR_DONE)
            samples_.append(header.lpData, std::min<size_t>(header.dwBytesRecorded, 32000 * 300 - samples_.size()));
    }
    for (auto& header : headers_) waveInUnprepareHeader(device_, &header, sizeof(header));
    waveInClose(device_); device_ = nullptr;
    auto result = waveFile(samples_); samples_.clear(); samples_.shrink_to_fit();
    return result;
}
void AudioRecorder::cancel() {
    if (device_) {
        waveInReset(device_);
        for (auto& header : headers_) if (header.dwFlags & WHDR_PREPARED) waveInUnprepareHeader(device_, &header, sizeof(header));
        waveInClose(device_); device_ = nullptr;
    }
    samples_.clear(); samples_.shrink_to_fit();
}
} // namespace mq
