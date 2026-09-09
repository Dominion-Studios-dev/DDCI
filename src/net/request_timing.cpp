#include "ddci/net/request_timing.hpp"

#include <algorithm>

namespace ddci::net {

namespace {

inline std::int64_t us_between(RequestTiming::clock::time_point begin,
                               RequestTiming::clock::time_point end) noexcept {
    if (begin.time_since_epoch().count() == 0) {
        return 0;
    }
    const auto d = std::chrono::duration_cast<std::chrono::microseconds>(
        end - begin);
    return std::max<std::int64_t>(0, d.count());
}

}

void RequestTiming::finalize() noexcept {
    queue_us = us_between(queue_enter, dns_start);

    if (ttfb.time_since_epoch().count() != 0) {
        ttfb_us = us_between(dns_start, ttfb);
    } else {
        ttfb_us = starttransfer_us;
    }

    if (ttft.time_since_epoch().count() != 0) {
        ttft_us = us_between(dns_start, ttft);
        generation_us = us_between(ttft, request_end);
    } else {
        ttft_us = ttfb_us;
        generation_us = 0;
    }

    total_us = us_between(dns_start, request_end);

    connection_reused =
        !cache_hit && dns_us <= kReusePhaseToleranceUs &&
        connect_us <= kReusePhaseToleranceUs &&
        tls_us <= kReusePhaseToleranceUs;
}

}
