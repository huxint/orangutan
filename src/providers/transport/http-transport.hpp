#pragma once

#include "providers/provider.hpp"

#include <functional>
#include <string>

namespace orangutan::providers::transport {

    using header_map = utils::transparent_string_unordered_map<std::string>;
    using sse_event_callback = std::function<void(std::string_view event_name, std::string_view data)>;

    struct HttpRequest {
        std::string url;
        std::string body;
        header_map headers;
        long timeout_seconds = 120;
    };

    struct HttpResponse {
        long status_code = 0;
        std::string body;
    };

    class HttpTransport {
    public:
        [[nodiscard]]
        HttpResponse post(const HttpRequest &request, const ModelTarget &target) const;

        [[nodiscard]]
        HttpResponse post_sse(const HttpRequest &request, const ModelTarget &target, const sse_event_callback &on_event) const;
    };

} // namespace orangutan::providers::transport
