#pragma once

#include "providers/protocols/protocol-adapter.hpp"

#include <memory>

namespace orangutan::providers::protocols {

    [[nodiscard]]
    std::shared_ptr<const ProtocolAdapter> make_openai_responses_adapter();

} // namespace orangutan::providers::protocols
