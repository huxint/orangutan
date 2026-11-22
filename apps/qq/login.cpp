#include "login.hpp"

#include <algorithm>
#include <array>
#include <asio/this_coro.hpp>
#include <nlohmann/json.hpp>
#include <oran/hook/bus.hpp>
#include <oran/io/blocking.hpp>
#include <sodium/utils.h>

namespace orangutan::qq_login {
namespace {
using core::Error;
using core::Result;
using Json = nlohmann::json;
struct Wipe {
  std::string& text;
  ~Wipe() {
    sodium_memzero(text.data(), text.size());
  }
};

async::Awaitable<Result<void>>
authorize(bootstrap::QQConnectOptions& options, std::string operation, core::Capability capability) {
  const auto cancellation = co_await asio::this_coro::cancellation_state;
  if (cancellation.cancelled() != asio::cancellation_type::none)
    co_return std::unexpected(Error::cancelled());
  const std::array required{capability};
  const auto decision = permission::evaluate(options.rules, operation, required, permission::Mode::strict);
  co_await options.hooks->publish_advisory(hook::Event::channel_action,
                                           hook::ChannelActionPayload{"qq",
                                                                      "onboarding",
                                                                      std::move(operation),
                                                                      decision.verdict == permission::Verdict::allow});
  if (decision.verdict != permission::Verdict::allow)
    co_return std::unexpected(Error::permission_denied("QQ credential storage denied"));
  co_return Result<void>{};
}

async::Awaitable<Result<std::string>> run_impl(bool probe,
                                               bootstrap::QQConnectOptions& options,
                                               io::PrivateDirectory& directory,
                                               asio::any_io_executor worker) {
  if (!options.hooks || !options.send || !worker)
    co_return std::unexpected(Error::invalid_argument("invalid QQ login services"));
  auto allowed = co_await authorize(options, "QQCredentialRead", core::Capability::read_file);
  if (!allowed)
    co_return std::unexpected(allowed.error());
  auto saved = co_await io::run_blocking(worker, [&directory](std::stop_token) {
    return directory.read("qq-credentials.json", 16384);
  });
  if (!saved)
    co_return std::unexpected(Error{saved.error().kind(), "cannot read QQ binding"});
  if (*saved) {
    Wipe wipe{**saved};
    if (!probe)
      co_return std::unexpected(Error{core::ErrorKind::conflict, "QQ binding already exists; use --probe"});
    auto json = Json::parse(**saved);
    auto app = json.at("app_id").get<std::string>();
    auto secret = json.at("app_secret").get<std::string>();
    Wipe secret_wipe{secret};
    Wipe json_wipe{json.at("app_secret").get_ref<std::string&>()};
    if (app.empty() || app.size() > 64 || secret.empty() || secret.size() > 4096 ||
        !std::ranges::all_of(app, [](char c) { return c >= '0' && c <= '9'; }) ||
        !std::ranges::all_of(secret, [](unsigned char c) { return c > 32 && c < 127; }))
      co_return std::unexpected(Error::parsing("invalid saved QQ binding"));
    auto source = bootstrap::QQTokenSource::create({.app_id = app,
                                                    .app_secret = std::move(secret),
                                                    .executor = co_await asio::this_coro::executor,
                                                    .send = options.send,
                                                    .hooks = options.hooks,
                                                    .rules = std::move(options.rules)});
    if (!source)
      co_return std::unexpected(source.error());
    auto token = co_await (*source)->get({channel::Platform::qq, app, channel::ChatKind::direct, {}, {}});
    if (!token)
      co_return std::unexpected(token.error());
    Wipe token_wipe{*token};
    co_return app;
  }
  if (probe)
    co_return std::unexpected(Error::not_found("no saved QQ binding"));
  // Fail closed before asking the user to authorize a bot when saving is forbidden.
  allowed = co_await authorize(options, "QQCredentialWrite", core::Capability::write_file);
  if (!allowed)
    co_return std::unexpected(allowed.error());
  auto credentials = co_await bootstrap::connect_qq(options);
  if (!credentials)
    co_return std::unexpected(credentials.error());
  Wipe secret_wipe{credentials->app_secret};
  allowed = co_await authorize(options, "QQCredentialWrite", core::Capability::write_file);
  if (!allowed)
    co_return std::unexpected(allowed.error());
  auto persist = [&directory, &credentials](std::stop_token) -> Result<void> {
    try {
      auto existing = directory.read("qq-credentials.json", 16384);
      if (!existing)
        return std::unexpected(existing.error());
      if (*existing) {
        Wipe wipe{**existing};
        return std::unexpected(Error{core::ErrorKind::conflict, "QQ binding already exists"});
      }
      Json value{{"app_id", credentials->app_id},
                 {"app_secret", credentials->app_secret},
                 {"user_openid", credentials->user_openid}};
      Wipe json_wipe{value.at("app_secret").get_ref<std::string&>()};
      auto bytes = value.dump() + "\n";
      Wipe wipe{bytes};
      return directory.write("qq-credentials.json", bytes);
    } catch (...) {
      return std::unexpected(Error::io("cannot save QQ binding"));
    }
  };
  auto written = co_await io::run_blocking(worker, std::move(persist));
  if (!written)
    co_return std::unexpected(Error{written.error().kind(), "cannot save QQ binding"});
  co_return std::move(credentials->app_id);
}
}  // namespace

async::Awaitable<Result<std::string>>
run(bool probe, bootstrap::QQConnectOptions options, io::PrivateDirectory& directory, asio::any_io_executor worker) {
  try {
    co_return co_await run_impl(probe, options, directory, std::move(worker));
  } catch (...) {}
  const auto cancellation = co_await asio::this_coro::cancellation_state;
  co_return std::unexpected(cancellation.cancelled() != asio::cancellation_type::none
                                ? Error::cancelled()
                                : Error::parsing("QQ login failed"));
}
}  // namespace orangutan::qq_login
