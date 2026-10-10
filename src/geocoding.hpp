#pragma once
#include "core.hpp"
#include <memory>

namespace mq {
std::string geocodingUrl(const std::string& endpoint, double latitude, double longitude);
std::string parseLocationName(const std::string& body);
// Call on an initialized COM UI thread whose apartment outlives this object.
// Polling keeps address lookup cancellable without blocking the editor.
class LocationNames {
public:
    LocationNames();
    ~LocationNames();
    void start(const std::string& endpoint, double latitude, double longitude);
    std::optional<std::string> poll();
    void cancel() noexcept;
    bool active() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<std::pair<std::string, std::string>> cache_;
    std::string key_;
    std::optional<std::string> ready_;
    ULONGLONG lastRequest_ = 0;
};
} // namespace mq
