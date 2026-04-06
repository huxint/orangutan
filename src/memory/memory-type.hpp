#pragma once

#include <cstdint>
#include <string_view>
#include "types/base.hpp"
#include <magic_enum/magic_enum.hpp>

namespace orangutan::memory {

    /// Semantic memory types inspired by Claude Code's taxonomy.
    enum class memory_type : base::u8 {
        /// User's role, goals, preferences, knowledge, and personal context.
        user,
        /// Corrections, validated approaches, and guidance on how to work.
        feedback,
        /// Ongoing work, decisions, deadlines, incidents, and project state.
        project,
        /// Pointers to external systems, docs, dashboards, and resources.
        reference,
    };

    /// Infer a memory_type from a legacy category string.
    [[nodiscard]]
    memory_type infer_memory_type(std::string_view category);

} // namespace orangutan::memory

namespace orangutan {

    using memory::infer_memory_type;
    using memory::memory_type;

} // namespace orangutan
