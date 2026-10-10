#include "location.hpp"
#include <winrt/Windows.Devices.Geolocation.h>
#include <winrt/Windows.Foundation.h>
#include <charconv>
#include <chrono>
#include <cmath>

namespace mq {
using namespace winrt::Windows::Devices::Geolocation;
using namespace winrt::Windows::Foundation;
namespace {
Error locationError(HRESULT result) {
    if (result == E_ACCESSDENIED)
        return Error("定位未获允许，请开启 Windows 位置权限。", ErrorKind::LocationDenied);
    if (result == HRESULT_FROM_WIN32(ERROR_TIMEOUT))
        return Error("定位超时，可重试或手动填写。", ErrorKind::Validation);
    return Error("暂时无法定位，可重试或手动填写。", ErrorKind::Validation);
}
}
std::wstring coordinateText(double value, CoordinateFormat format) {
    if (!std::isfinite(value)) throw Error("坐标无效。", ErrorKind::Validation);
    char buffer[64]{};
    const auto result = format == CoordinateFormat::Full
        ? std::to_chars(buffer, buffer + sizeof(buffer), value)
        : std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::fixed, 6);
    if (result.ec != std::errc{}) throw Error("无法显示坐标。", ErrorKind::Validation);
    return wide(std::string(buffer, result.ptr));
}
struct LocationReader::Impl {
    // WinRT is initialized only on an explicit location request. Apartment is
    // destroyed last, after every WinRT object has been released on the UI thread.
    struct Apartment {
        Apartment() { winrt::init_apartment(winrt::apartment_type::single_threaded); }
        ~Apartment() { winrt::uninit_apartment(); }
    } apartment;
    IAsyncOperation<GeolocationAccessStatus> access{nullptr};
    Geolocator locator{nullptr};
    IAsyncOperation<Geoposition> position{nullptr};
    ULONGLONG started = 0;
    ~Impl() {
        try { if (access) access.Cancel(); } catch (...) {}
        try { if (position) position.Cancel(); } catch (...) {}
    }
};
LocationReader::LocationReader() = default;
LocationReader::~LocationReader() = default;
bool LocationReader::active() const { return impl_ != nullptr; }
void LocationReader::cancel() noexcept { impl_.reset(); }
void LocationReader::start() {
    cancel();
    try {
        auto next = std::make_unique<Impl>();
        next->started = GetTickCount64();
        // Called by the foreground window's button handler, never by a worker.
        next->access = Geolocator::RequestAccessAsync();
        impl_ = std::move(next);
    } catch (const winrt::hresult_error& error) { throw locationError(error.code()); }
}
std::optional<LocationFix> LocationReader::poll() {
    if (!impl_) return {};
    try {
        if (GetTickCount64() - impl_->started >= 60000)
            throw Error("定位超时，可重试或手动填写。", ErrorKind::Validation);
        if (impl_->access) {
            if (impl_->access.Status() == AsyncStatus::Started) return {};
            const auto status = impl_->access.GetResults();
            impl_->access = nullptr;
            if (status == GeolocationAccessStatus::Denied) throw locationError(E_ACCESSDENIED);
            if (status != GeolocationAccessStatus::Allowed)
                throw Error("Windows 定位尚未就绪，可重试或手动填写。", ErrorKind::Validation);
            impl_->locator = Geolocator();
            impl_->locator.DesiredAccuracy(PositionAccuracy::Default);
            // Reuse a recent fix for responsiveness and bound the wait for a new one.
            impl_->position = impl_->locator.GetGeopositionAsync(std::chrono::minutes(2), std::chrono::seconds(15));
        }
        if (impl_->position.Status() == AsyncStatus::Started) return {};
        LocationFix fix;
        {
            const auto coordinate = impl_->position.GetResults().Coordinate();
            const auto point = coordinate.Point().Position();
            fix = {point.Latitude, point.Longitude, coordinate.Accuracy(),
                   coordinate.PositionSource() == PositionSource::Default};
        }
        if (!validLocation({"当前位置", fix.latitude, fix.longitude}) || !std::isfinite(fix.accuracy) || fix.accuracy < 0)
            throw Error("Windows 返回了无效位置，可重试或手动填写。", ErrorKind::Validation);
        cancel();
        return fix;
    } catch (const winrt::hresult_error& error) {
        cancel();
        throw locationError(error.code());
    } catch (...) { cancel(); throw; }
}
} // namespace mq
