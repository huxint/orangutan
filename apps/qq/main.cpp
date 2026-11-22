#include "login.hpp"

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/signal_set.hpp>
#include <csignal>
#include <filesystem>
#include <oran/async/runtime.hpp>
#include <oran/hook/bus.hpp>
#include <print>
#include <qrencode.h>

namespace {
using namespace orangutan;
void usage() {
  std::println("oran-qq-login --state DIRECTORY --workspace DIRECTORY [--probe]\n"
               "Scan with mobile QQ to bind an official bot; no AppID/AppSecret input is needed.\n"
               "State must be outside the agent workspace. Existing bindings are never replaced.\n"
               "--probe verifies saved credentials without creating a QR or sending messages.\n"
               "This command binds credentials; it does not start a QQ message receiver.");
}

async::Awaitable<core::Result<void>> display(std::string url) {
  auto qr = std::unique_ptr<QRcode, decltype(&QRcode_free)>{QRcode_encodeString8bit(url.c_str(), 0, QR_ECLEVEL_M),
                                                            QRcode_free};
  if (!qr)
    co_return std::unexpected(core::Error::internal("cannot render QQ QR"));
  const auto dark = [&qr](int x, int y) {
    return x >= 0 && y >= 0 && x < qr->width && y < qr->width && (qr->data[y * qr->width + x] & 1) != 0;
  };
  std::println("Scan with mobile QQ to authorize binding (Ctrl-C cancels):");
  for (int y = -4; y < qr->width + 4; y += 2) {
    std::print("\033[30;47m");
    for (int x = -4; x < qr->width + 4; ++x) {
      const bool top = dark(x, y), bottom = dark(x, y + 1);
      std::print("{}", top ? (bottom ? "█" : "▀") : (bottom ? "▄" : " "));
    }
    std::println("\033[0m");
  }
  std::println("{}", url);
  std::fflush(stdout);
  co_return core::Result<void>{};
}
}  // namespace

int main(int argc, char** argv) try {
  std::string state_path, workspace;
  bool probe = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument{argv[i]};
    if (argument == "--help") {
      usage();
      return 0;
    }
    if (argument == "--probe") {
      probe = true;
      continue;
    }
    if (++i >= argc) {
      usage();
      return 2;
    }
    if (argument == "--state")
      state_path = argv[i];
    else if (argument == "--workspace")
      workspace = argv[i];
    else {
      usage();
      return 2;
    }
  }
  if (state_path.empty() || workspace.empty()) {
    usage();
    return 2;
  }
  const auto root = std::filesystem::canonical(workspace);
  const auto state = std::filesystem::weakly_canonical(state_path);
  const auto relative = state.lexically_relative(root);
  if (relative.empty() || *relative.begin() != "..") {
    std::println(stderr, "QQ credentials must be outside the agent workspace");
    return 2;
  }
  auto directory = io::PrivateDirectory::open(state.string());
  if (!directory) {
    std::println(stderr, "Cannot open private QQ state");
    return 2;
  }
  auto lock = directory->lock("login.lock");
  if (!lock) {
    std::println(stderr, "QQ binding directory is already in use");
    return 2;
  }
  async::Runtime runtime;
  auto strand = runtime.make_strand();
  http::Client http{runtime.cpu_executor()};
  hook::Bus hooks;
  bootstrap::QQConnectOptions options;
  options.send = [&http](http::BodyRequest request) {
    return http.send(std::move(request));
  };
  options.hooks = &hooks;
  options.display = display;
  for (const auto* operation : {"QQBind", "QQToken", "QQCredentialRead", "QQCredentialWrite"})
    options.rules.push_back({.verdict = permission::Verdict::allow, .tool_pattern = operation});
  asio::cancellation_signal cancellation;
  asio::signal_set signals{strand, SIGINT, SIGTERM};
  signals.async_wait([&](const asio::error_code& error, int) {
    if (!error)
      cancellation.emit(asio::cancellation_type::all);
  });
  int exit_code = 1;
  asio::co_spawn(
      strand,
      qq_login::run(probe, std::move(options), *directory, runtime.cpu_executor()),
      asio::bind_cancellation_slot(
          cancellation.slot(),
          [&](std::exception_ptr exception, core::Result<std::string> result) {
            if (!exception && result) {
              std::println("QQ bot {}: {}", *result, probe ? "credentials verified" : "binding saved");
              exit_code = 0;
            } else {
              std::println(stderr, "QQ login stopped: {}", exception ? "internal" : result.error().message());
            }
            signals.cancel();
            runtime.stop();
          }));
  return runtime.run() ? exit_code : 1;
} catch (...) {
  std::println(stderr, "QQ login setup failed");
  return 2;
}
