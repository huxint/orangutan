#pragma once

#include "providers/provider.hpp"
#include "providers/transport/http-transport.hpp"

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

    class AuthStrategy {
    public:
        virtual ~AuthStrategy() = default;

        AuthStrategy(const AuthStrategy &) = delete;
        AuthStrategy &operator=(const AuthStrategy &) = delete;
        AuthStrategy(AuthStrategy &&) = delete;
        AuthStrategy &operator=(AuthStrategy &&) = delete;

        virtual void apply(const ModelTarget &target, transport::header_map &headers) const = 0;

    protected:
        AuthStrategy() = default;
    };

    struct ProviderAssembly {
        std::shared_ptr<const ProtocolAdapter> adapter;
        std::shared_ptr<const AuthStrategy> auth;
    };

} // namespace orangutan::providers::protocols
