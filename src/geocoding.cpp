#include "geocoding.hpp"
#include "location.hpp"
#include "json.hpp"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <algorithm>

namespace mq {
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::Web::Http;
using namespace winrt::Windows::Web::Http::Filters;
namespace {
Error lookupError() { return Error(i18n::tr("error.geocoding_failed"), ErrorKind::Network); }
}
std::string geocodingUrl(const std::string& endpoint, double latitude, double longitude) {
    const auto input = trim(endpoint);
    const auto server = parseServer(input);
    if (!server.secure && server.host != L"127.0.0.1" && server.host != L"localhost" && server.host != L"[::1]")
        throw Error(i18n::tr("error.geocoding_https"), ErrorKind::Validation);
    if (!validLocation({"位置", latitude, longitude})) throw Error(i18n::tr("error.coords_invalid"), ErrorKind::Validation);
    return input + "?format=json&accept-language=zh-CN&lat=" + utf8(coordinateText(latitude, CoordinateFormat::Rounded))
        + "&lon=" + utf8(coordinateText(longitude, CoordinateFormat::Rounded));
}
std::string parseLocationName(const std::string& body) {
    if (body.size() > 65536) throw lookupError();
    const auto value = nlohmann::json::parse(body, nullptr, false);
    if (!value.is_object() || !value.contains("display_name") || !value["display_name"].is_string()) throw lookupError();
    const auto name = trim(value["display_name"].get<std::string>());
    if (!validLocation({name, 0, 0}) || name.find_first_of("\r\n\t") != std::string::npos) throw lookupError();
    wide(name); // Reject invalid UTF-8 before it reaches native controls.
    return name;
}
struct LocationNames::Impl {
    struct Apartment {
        Apartment() { winrt::init_apartment(winrt::apartment_type::single_threaded); }
        ~Apartment() { winrt::uninit_apartment(); }
    } apartment;
    HttpBaseProtocolFilter filter;
    HttpClient client{nullptr};
    IAsyncOperationWithProgress<HttpResponseMessage, HttpProgress> response{nullptr};
    IAsyncOperationWithProgress<IInputStream, uint64_t> streamOperation{nullptr};
    IInputStream stream{nullptr};
    IAsyncOperationWithProgress<IBuffer, uint32_t> read{nullptr};
    std::string body;
    ULONGLONG started = GetTickCount64();
    ~Impl() {
        try { if (response) response.Cancel(); } catch (...) {}
        try { if (streamOperation) streamOperation.Cancel(); } catch (...) {}
        try { if (read) read.Cancel(); } catch (...) {}
        try { if (stream) stream.Close(); } catch (...) {}
        try { if (client) client.Close(); } catch (...) {}
    }
};
LocationNames::LocationNames() = default;
LocationNames::~LocationNames() = default;
bool LocationNames::active() const { return impl_ || ready_.has_value(); }
void LocationNames::cancel() noexcept { impl_.reset(); ready_.reset(); }
void LocationNames::start(const std::string& endpoint, double latitude, double longitude) {
    cancel();
    key_ = geocodingUrl(endpoint, latitude, longitude);
    const auto cached = std::find_if(cache_.begin(), cache_.end(), [&](const auto& entry) { return entry.first == key_; });
    if (cached != cache_.end()) { ready_ = cached->second; return; }
    // User-triggered lookups only; never retry or send more than one per second.
    const auto now = GetTickCount64();
    if (lastRequest_ && now - lastRequest_ < 1000) throw Error(i18n::tr("error.geocoding_rate"), ErrorKind::Validation);
    lastRequest_ = now;
    try {
        auto next = std::make_unique<Impl>();
        next->filter.AllowAutoRedirect(false);
        next->filter.AllowUI(false);
        next->filter.CookieUsageBehavior(HttpCookieUsageBehavior::NoCookies);
        next->filter.CacheControl().ReadBehavior(HttpCacheReadBehavior::NoCache);
        next->filter.CacheControl().WriteBehavior(HttpCacheWriteBehavior::NoCache);
        next->client = HttpClient(next->filter);
        next->client.DefaultRequestHeaders().TryAppendWithoutValidation(L"User-Agent", L"MemoQuick/0.2.4 (https://github.com/alksv1/memoquick)");
        next->client.DefaultRequestHeaders().TryAppendWithoutValidation(L"Accept", L"application/json");
        next->response = next->client.GetAsync(Uri(wide(key_)), HttpCompletionOption::ResponseHeadersRead);
        impl_ = std::move(next);
    } catch (const winrt::hresult_error&) { throw lookupError(); }
}
std::optional<std::string> LocationNames::poll() {
    if (ready_) { auto result = std::move(ready_); ready_.reset(); return result; }
    if (!impl_) return {};
    try {
        if (GetTickCount64() - impl_->started > 8000) throw lookupError();
        if (impl_->response) {
            if (impl_->response.Status() == AsyncStatus::Started) return {};
            const auto response = impl_->response.GetResults();
            if (!response.IsSuccessStatusCode()) throw lookupError();
            const auto length = response.Content().Headers().ContentLength();
            if (length && length.Value() > 65536) throw lookupError();
            impl_->streamOperation = response.Content().ReadAsInputStreamAsync();
            impl_->response = nullptr;
        }
        if (impl_->streamOperation) {
            if (impl_->streamOperation.Status() == AsyncStatus::Started) return {};
            impl_->stream = impl_->streamOperation.GetResults();
            impl_->streamOperation = nullptr;
            impl_->read = impl_->stream.ReadAsync(Buffer(4096), 4096, InputStreamOptions::Partial);
        }
        if (impl_->read.Status() == AsyncStatus::Started) return {};
        {
            const auto buffer = impl_->read.GetResults();
            if (buffer.Length()) {
                impl_->body.append(reinterpret_cast<const char*>(buffer.data()), buffer.Length());
                if (impl_->body.size() > 65536) throw lookupError();
                impl_->read = impl_->stream.ReadAsync(Buffer(4096), 4096, InputStreamOptions::Partial);
                return {};
            }
        }
        auto name = parseLocationName(impl_->body);
        if (cache_.size() >= 100) cache_.erase(cache_.begin());
        cache_.emplace_back(key_, name);
        cancel();
        return name;
    } catch (const winrt::hresult_error&) { cancel(); throw lookupError(); }
    catch (...) { cancel(); throw; }
}
} // namespace mq
