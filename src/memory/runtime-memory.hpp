#pragma once

#include "types/types.hpp"
#include "memory/memory-store.hpp"
#include "memory/memory-type.hpp"
#include "bootstrap/memory-context.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace orangutan::memory {

    class RuntimeMemory {
    public:
        RuntimeMemory(MemoryStore &store, bootstrap::RuntimeMemoryContext context = {});

        [[nodiscard]]
        const bootstrap::RuntimeMemoryContext &context() const {
            return context_;
        }

        void remember(std::string_view key, std::string_view content, memory_type kind = memory_type::user);

        [[nodiscard]]
        bool forget(std::string_view key);

        [[nodiscard]]
        std::vector<MemoryRecord> recall_records(std::string_view query, std::size_t limit = 8);

        [[nodiscard]]
        std::string recall(std::string_view query, std::size_t limit = 8);

    private:
        MemoryStore *store_ = nullptr;
        bootstrap::RuntimeMemoryContext context_;
    };

} // namespace orangutan::memory

namespace orangutan {

    using memory::RuntimeMemory;

} // namespace orangutan
