#pragma once

#include <cstdint>
#include <magic_enum/magic_enum.hpp>

namespace orangutan::memory {

    /// Semantic memory types inspired by Claude Code's taxonomy.
    enum class memory_type : std::uint8_t {
        /// User's role, goals, preferences, knowledge, and personal context.
        user,
        /// Corrections, validated approaches, and guidance on how to work.
        feedback,
        /// Ongoing work, decisions, deadlines, incidents, and project state.
        project,
        /// Pointers to external systems, docs, dashboards, and resources.
        reference,
    };

} // namespace orangutan::memory

namespace orangutan {

    using memory::memory_type;

} // namespace orangutan
