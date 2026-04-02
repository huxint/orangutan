#include "memory/memory-type.hpp"

#include <uni_algo/case.h>

namespace orangutan::memory {

    MemoryType infer_memory_type(std::string_view category) {
        const auto lower = una::cases::to_lowercase_utf8(category);

        // Direct type names
        if (const auto direct = magic_enum::enum_cast<MemoryType>(lower, magic_enum::case_insensitive); direct.has_value()) {
            return *direct;
        }

        // Legacy category -> type mapping
        if (lower == "profile" || lower == "preference" || lower == "general") {
            return MemoryType::user;
        }
        if (lower == "decision" || lower == "task" || lower == "journal") {
            return MemoryType::project;
        }
        if (lower == "learning") {
            return MemoryType::feedback;
        }
        if (lower == "fact") {
            return MemoryType::reference;
        }

        return MemoryType::user;
    }

} // namespace orangutan::memory
