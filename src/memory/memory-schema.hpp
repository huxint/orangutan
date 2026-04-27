#pragma once

#include "storage/sqlite.hpp"

#include <filesystem>

namespace orangutan::memory::detail {

    [[nodiscard]]
    std::filesystem::path default_db_path();
    void create_current_schema(sqlite::Database &db);

} // namespace orangutan::memory::detail
