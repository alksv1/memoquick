#pragma once
#include "core.hpp"
#include <memory>

namespace mq {
struct LocationFix {
    double latitude = 0, longitude = 0, accuracy = 0;
    bool isDefault = false;
};
enum class CoordinateFormat { Full, Rounded };
std::wstring coordinateText(double value, CoordinateFormat format = CoordinateFormat::Full);
class LocationReader {
public:
    LocationReader();
    ~LocationReader();
    LocationReader(const LocationReader&) = delete;
    LocationReader& operator=(const LocationReader&) = delete;
    void start();
    std::optional<LocationFix> poll();
    void cancel() noexcept;
    bool active() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace mq
