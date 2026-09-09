#include "ddci/net/connection_manager.hpp"

#include <cpr/cpr.h>
#include <curl/curl.h>

#include <algorithm>
#include <cstdint>

namespace ddci::net {

ConnectionManager::ConnectionManager(const core::Config& config)
    : config_(config)
    , endpoint_(config.groq_api_url)
    , auth_header_("Bearer " + config.groq_api_key) {
    completion_session_ = std::make_unique<cpr::Session>();
    stream_session_ = std::make_unique<cpr::Session>();

    for (auto* s : {completion_session_.get(), stream_session_.get()}) {
        s->SetTimeout(cpr::Timeout{kRequestTimeoutMs});
        s->SetSslOptions(
            cpr::Ssl(cpr::ssl::VerifyHost{true}, cpr::ssl::VerifyPeer{true}));
    }

    apply_idle_timeout();
}

ConnectionManager::~ConnectionManager() = default;

cpr::Response ConnectionManager::perform(
    cpr::Session& session, const nlohmann::json& payload,
    RequestTiming& timing) {
    session.SetOption(cpr::Url{endpoint_});
    session.SetOption(cpr::Header{
        {"Authorization", auth_header_},
        {"Content-Type", "application/json"}
    });
    session.SetOption(cpr::Body{payload.dump()});

    session.PreparePost();
    timing.dns_start = RequestTiming::clock::now();
    const CURLcode code = curl_easy_perform(session.GetCurlHolder()->handle);
    timing.request_end = RequestTiming::clock::now();
    cpr::Response r = session.Complete(code);

    CURL* handle = session.GetCurlHolder()->handle;
    extract_curl_timing(handle, timing);

    update_adaptive_timeout(timing.request_end);
    return r;
}

cpr::Response ConnectionManager::post(const nlohmann::json& payload,
                                      RequestTiming& timing) {
    return perform(*completion_session_, payload, timing);
}

cpr::Response ConnectionManager::post_stream(
    const nlohmann::json& payload, cpr::WriteCallback callback,
    RequestTiming& timing) {
    stream_session_->SetOption(callback);
    return perform(*stream_session_, payload, timing);
}

void ConnectionManager::extract_curl_timing(CURL* handle,
                                            RequestTiming& timing) noexcept {
    if (handle == nullptr) {
        return;
    }
    const auto phase_us = [handle](CURLINFO info) -> std::int64_t {
        curl_off_t v = 0;
        if (curl_easy_getinfo(handle, info, &v) == CURLE_OK) {
            return static_cast<std::int64_t>(v);
        }
        return 0;
    };

    timing.dns_us = phase_us(CURLINFO_NAMELOOKUP_TIME_T);
    const std::int64_t connect = phase_us(CURLINFO_CONNECT_TIME_T);
    const std::int64_t appconnect = phase_us(CURLINFO_APPCONNECT_TIME_T);
    timing.setup_us = appconnect;
    timing.starttransfer_us = phase_us(CURLINFO_STARTTRANSFER_TIME_T);
    timing.connect_us = std::max<std::int64_t>(0, connect - timing.dns_us);
    timing.tls_us = std::max<std::int64_t>(0, appconnect - connect);

    curl_off_t nbytes = 0;
    if (curl_easy_getinfo(handle, CURLINFO_SIZE_DOWNLOAD_T, &nbytes) ==
        CURLE_OK) {
        timing.response_bytes = static_cast<std::uint64_t>(nbytes);
    }
}

void ConnectionManager::update_adaptive_timeout(
    RequestTiming::clock::time_point end) noexcept {
    if (have_gap_) {
        const double gap_us = std::chrono::duration_cast<
            std::chrono::microseconds>(end - last_request_end_).count();
        const double gap = std::max<double>(0.0, gap_us);
        ema_gap_us_ = kEmaAlpha * gap + (1.0 - kEmaAlpha) * ema_gap_us_;
    } else {
        ema_gap_us_ = static_cast<double>(kInitialIdleTimeoutMs) * 1000.0;
        have_gap_ = true;
    }
    last_request_end_ = end;

    const double target_ms =
        std::clamp(ema_gap_us_ * kIdleScale / 1000.0,
                   static_cast<double>(kMinIdleMs),
                   static_cast<double>(kMaxIdleMs));
    const std::int64_t next = static_cast<std::int64_t>(target_ms);
    if (next != idle_timeout_ms_) {
        idle_timeout_ms_ = next;
        apply_idle_timeout();
    }
}

void ConnectionManager::apply_idle_timeout() noexcept {
    const long seconds = static_cast<long>(idle_timeout_ms_ / 1000);
    for (auto* s : {completion_session_.get(), stream_session_.get()}) {
        CURL* h = (s != nullptr) ? s->GetCurlHolder()->handle : nullptr;
        if (h == nullptr) {
            continue;
        }
        curl_easy_setopt(h, CURLOPT_MAXAGE_CONN, std::max<long>(1L, seconds));
        curl_easy_setopt(h, CURLOPT_MAXLIFETIME_CONN,
                         std::max<long>(1L, seconds + 60L));
        curl_easy_setopt(h, CURLOPT_FORBID_REUSE, 0L);
    }
}

}
