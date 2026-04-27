#include "providers/protocols/protocol-json.hpp"

#include <string>

#include "providers/provider.hpp"

namespace orangutan::providers::protocols {
    namespace {

        [[nodiscard]]
        ProviderError make_protocol_json_error(std::string_view protocol_label, std::string_view context, std::string_view detail) {
            return ProviderError(error_category::parsing, std::string(protocol_label) + " " + std::string(context) + ": " + std::string(detail));
        }

    } // namespace

    nlohmann::json parse_protocol_json_object(std::string_view payload, std::string_view protocol_label, std::string_view context) {
        try {
            auto parsed = nlohmann::json::parse(payload);
            if (!parsed.is_object()) {
                throw make_protocol_json_error(protocol_label, context, "expected a json object");
            }
            return parsed;
        } catch (const ProviderError &) {
            throw;
        } catch (const nlohmann::json::exception &error) {
            throw make_protocol_json_error(protocol_label, context, error.what());
        }
    }

} // namespace orangutan::providers::protocols
