#include "memory/runtime-memory.hpp"

namespace orangutan::memory {

    RuntimeMemory::RuntimeMemory(MemoryStore &store, bootstrap::RuntimeMemoryContext context)
    : store_(&store),
      context_(std::move(context)) {}

    void RuntimeMemory::remember(std::string_view key, std::string_view content, memory_type kind) {
        store_->remember(key, content, kind, context_.scope);
    }

    bool RuntimeMemory::forget(std::string_view key) {
        return store_->forget(key, context_.scope);
    }

    std::vector<MemoryRecord> RuntimeMemory::recall_records(std::string_view query, std::size_t limit) {
        return store_->search(query, context_.scope, limit);
    }

    std::string RuntimeMemory::recall(std::string_view query, std::size_t limit) {
        return store_->recall(query, context_.scope, limit);
    }

} // namespace orangutan::memory
