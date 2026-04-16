#pragma once

#include "providers/provider.hpp"
#include "providers/transport/http-transport.hpp"

#include <functional>
#include <memory>

namespace orangutan::providers::protocols {

    class StreamDecoder {
    public:
        virtual ~StreamDecoder() = default;

        StreamDecoder(const StreamDecoder &) = delete;
        StreamDecoder &operator=(const StreamDecoder &) = delete;
        StreamDecoder(StreamDecoder &&) = delete;
        StreamDecoder &operator=(StreamDecoder &&) = delete;

        virtual void on_event(std::string_view event_name, std::string_view payload) = 0;

        [[nodiscard]]
        virtual LLMResponse finish() const = 0;

    protected:
        StreamDecoder() = default;
    };

    class ProtocolAdapter {
    public:
        virtual ~ProtocolAdapter() = default;

        ProtocolAdapter(const ProtocolAdapter &) = delete;
        ProtocolAdapter &operator=(const ProtocolAdapter &) = delete;
        ProtocolAdapter(ProtocolAdapter &&) = delete;
        ProtocolAdapter &operator=(ProtocolAdapter &&) = delete;

        [[nodiscard]]
        virtual transport::HttpRequest build_request(const ModelTarget &target, const ProviderRequest &request) const = 0;

        [[nodiscard]]
        virtual LLMResponse parse_response(const transport::HttpResponse &response) const = 0;

        [[nodiscard]]
        virtual std::unique_ptr<StreamDecoder> make_stream_decoder(const ProviderEventSink &sink) const = 0;

        [[nodiscard]]
        virtual std::string label() const = 0;

    protected:
        ProtocolAdapter() = default;
    };

    using AuthFn = std::function<void(const ModelTarget &, transport::header_map &)>;

    struct ProviderAssembly {
        std::shared_ptr<const ProtocolAdapter> adapter;
        AuthFn auth;
    };

} // namespace orangutan::providers::protocols
