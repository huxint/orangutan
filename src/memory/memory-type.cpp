#include "memory/memory-type.hpp"

#include <uni_algo/case.h>

namespace orangutan::memory {

    memory_type infer_memory_type(std::string_view category) {
        const auto lower = una::cases::to_lowercase_utf8(category);

        // Direct type names
        if (const auto direct = magic_enum::enum_cast<memory_type>(lower, magic_enum::case_insensitive); direct.has_value()) {
            return *direct;
        }

        // Legacy category -> type mapping
        if (lower == "profile" || lower == "preference" || lower == "general") {
            return memory_type::user;
        }
        if (lower == "decision" || lower == "task" || lower == "journal") {
            return memory_type::project;
        }
        if (lower == "learning") {
            return memory_type::feedback;
        }
        if (lower == "fact") {
            return memory_type::reference;
        }

        return memory_type::user;
    }

} // namespace orangutan::memory
