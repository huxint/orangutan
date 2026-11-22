#pragma once

#include <oran/bootstrap/qq_connect.hpp>
#include <oran/io/private_directory.hpp>

namespace orangutan::qq_login {
/// The host holds the directory lock until this operation and all worker work finish.
/// Returns only the bound AppID; credential values remain in the private file.
[[nodiscard]] async::Awaitable<core::Result<std::string>>
run(bool probe, bootstrap::QQConnectOptions options, io::PrivateDirectory& directory, asio::any_io_executor worker);
}  // namespace orangutan::qq_login
