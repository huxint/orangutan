#include "local_provider.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <asio/this_coro.hpp>
#include <nlohmann/json.hpp>

#include <oran/async/sleep.hpp>
#include <oran/core/content.hpp>
#include <oran/core/message.hpp>
#include <oran/core/role.hpp>
#include <oran/core/stop_reason.hpp>
#include <oran/tool/builtins.hpp>

namespace orangutan::personal {
namespace {

[[nodiscard]] std::string extract_text(const core::Message& msg) {
  std::string out;
  for (const auto& block : msg.blocks) {
    if (const auto* text = std::get_if<core::TextContent>(&block)) {
      if (!out.empty()) {
        out.push_back('\n');
      }
      out += text->text;
    }
  }
  return out;
}

[[nodiscard]] bool has_tool_result_after_last_user(const provider::Request& request, std::string& last_tool_output,
                                                   bool& last_tool_is_error, std::string& last_tool_name) {
  std::size_t last_user_idx = 0;
  bool found_user = false;
  for (std::size_t i = 0; i < request.messages.size(); ++i) {
    if (request.messages[i].role == core::Role::user) {
      bool is_tool_result = false;
      for (const auto& b : request.messages[i].blocks) {
        if (std::holds_alternative<core::ToolResultContent>(b)) {
          is_tool_result = true;
          break;
        }
      }
      if (!is_tool_result) {
        last_user_idx = i;
        found_user = true;
      }
    }
  }
  if (!found_user) {
    return false;
  }
  for (std::size_t i = last_user_idx + 1; i < request.messages.size(); ++i) {
    for (const auto& b : request.messages[i].blocks) {
      if (const auto* use = std::get_if<core::ToolUseContent>(&b)) {
        last_tool_name = use->name;
      } else if (const auto* res = std::get_if<core::ToolResultContent>(&b)) {
        last_tool_output = res->output;
        last_tool_is_error = res->is_error;
        return true;
      }
    }
  }
  return false;
}

[[nodiscard]] std::string last_user_prompt(const provider::Request& request) {
  for (auto it = request.messages.rbegin(); it != request.messages.rend(); ++it) {
    if (it->role != core::Role::user) {
      continue;
    }
    bool is_tool_result = false;
    for (const auto& b : it->blocks) {
      if (std::holds_alternative<core::ToolResultContent>(b)) {
        is_tool_result = true;
        break;
      }
    }
    if (!is_tool_result) {
      return extract_text(*it);
    }
  }
  return {};
}

[[nodiscard]] bool has_tool(const provider::Request& request, std::string_view name) {
  return std::ranges::any_of(request.tools, [name](const core::ToolDef& def) { return def.name == name; });
}

[[nodiscard]] bool contains_any(std::string_view haystack, std::initializer_list<std::string_view> needles) {
  for (const auto needle : needles) {
    if (haystack.contains(needle)) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] async::Awaitable<void> emit_streaming_text(std::string_view text, provider::EventSink* sink) {
  if (sink == nullptr) {
    co_return;
  }
  const auto ex = co_await asio::this_coro::executor;
  constexpr std::size_t kChunk = 24;
  std::size_t pos = 0;
  while (pos < text.size()) {
    std::size_t end = std::min(text.size(), pos + kChunk);
    while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) {
      ++end;
    }
    sink->on_text_delta(text.substr(pos, end - pos));
    pos = end;
    static_cast<void>(co_await async::sleep_for(ex, std::chrono::milliseconds{18}));
  }
}

}  // namespace

async::Awaitable<core::Result<provider::Response>>
LocalDemoProvider::send(provider::Request request, provider::ModelTarget target, provider::EventSink* sink) const {
  const auto prompt = last_user_prompt(request);
  const auto model_name = target.model.empty() ? std::string{"orangutan-local-demo"} : target.model;

  std::string tool_output;
  bool tool_error = false;
  std::string tool_name;
  const bool after_tool = has_tool_result_after_last_user(request, tool_output, tool_error, tool_name);

  if (!after_tool) {
    const auto call_id = std::format("local-call-{}", ++call_seq_);

    if (contains_any(prompt, {"记住", "偏好", "我喜欢", "remember", "prefer"}) &&
        has_tool(request, tool::kMemoryRememberName)) {
      const nlohmann::json args{
          {"id", std::format("pref-{}", call_seq_.load())},
          {"kind", "user"},
          {"title", prompt.substr(0, std::min<std::size_t>(prompt.size(), 48))},
          {"body", prompt},
          {"importance", 0.8},
          {"tags", nlohmann::json::array({"personal", "preference"})},
      };
      provider::Response resp;
      resp.model_used = model_name;
      resp.stop_reason = core::StopReason::tool_use;
      resp.blocks.push_back(core::ToolUseContent{
          .id = call_id,
          .name = std::string{tool::kMemoryRememberName},
          .input_json = args.dump(),
      });
      if (sink != nullptr) {
        sink->on_tool_start(call_id, tool::kMemoryRememberName);
        sink->on_done(resp.stop_reason);
      }
      co_return resp;
    }

    if (contains_any(prompt, {"写入", "整理", "创建笔记", "write", "organize"}) &&
        has_tool(request, tool::kFileWriteName)) {
      const nlohmann::json args{
          {"path", "orangutan-note.md"},
          {"content", std::format("# 个人整理笔记\n\n请求：{}\n\n- 已按工作区权限边界整理输出。\n", prompt)},
          {"mode", "truncate"},
      };
      provider::Response resp;
      resp.model_used = model_name;
      resp.stop_reason = core::StopReason::tool_use;
      resp.blocks.push_back(core::ToolUseContent{
          .id = call_id,
          .name = std::string{tool::kFileWriteName},
          .input_json = args.dump(),
      });
      if (sink != nullptr) {
        sink->on_tool_start(call_id, tool::kFileWriteName);
        sink->on_done(resp.stop_reason);
      }
      co_return resp;
    }

    if (contains_any(prompt, {"列出", "目录", "文件", "搜索", "list", "files", "workspace", "shell"}) &&
        has_tool(request, tool::kShellExecName)) {
      provider::Response resp;
      resp.model_used = model_name;
      resp.stop_reason = core::StopReason::tool_use;
      resp.blocks.push_back(core::ToolUseContent{
          .id = call_id,
          .name = std::string{tool::kShellExecName},
          .input_json = R"({"command":"ls -la","timeout_ms":10000})",
      });
      if (sink != nullptr) {
        sink->on_tool_start(call_id, tool::kShellExecName);
        sink->on_done(resp.stop_reason);
      }
      co_return resp;
    }
  }

  std::string answer;
  if (after_tool) {
    if (tool_error) {
      answer = std::format("操作 `{}` 未执行：{}\n\n原始工作区文件保持不变。", tool_name, tool_output);
    } else if (tool_name == tool::kMemoryRememberName) {
      answer = "已将你的这条偏好写入本地 SQLite 长期记忆库，后续新对话也会自动发现并参考。";
    } else if (tool_name == tool::kFileWriteName) {
      answer = "已通过权限确认并写入 `orangutan-note.md`。你可以随时打开检查或继续补充。";
    } else if (tool_name == tool::kShellExecName) {
      answer = std::format("我通过 `ShellExec` 执行命令并拿到了当前工作区结果：\n\n```\n{}\n```\n\n需要小队继续拆解下一步任务吗？",
                           tool_output);
    } else {
      answer = std::format("已完成 `{}`：\n\n{}", tool_name, tool_output);
    }
  } else {
    answer = std::format(
        "你好，我是 **Oran**（Orangutan 本地个人助手 · 离线演示模型）。\n\n"
        "你刚才说：「{}」\n\n"
        "当前已连接真实的 C++26 运行时与 SQLite 存储（精简工具面：文件读写 + `ShellExec` 自主组合命令）：\n"
        "- 说「**列出工作区文件**」通过 `ShellExec` 执行 `ls -la`（带权限确认）；\n"
        "- 说「**整理并写入笔记**」触发真实 `FileWrite` 与交互权限审批；\n"
        "- 说「**记住：以后回复尽量简短**」写入真实长期记忆库。\n\n"
        "配置 `--config config.example.json` 并提供 API Key 后即可切换到真实的 Anthropic / OpenAI / DeepSeek 模型。",
        prompt);
  }

  co_await emit_streaming_text(answer, sink);

  provider::Response resp;
  resp.model_used = model_name;
  resp.stop_reason = core::StopReason::end_turn;
  resp.usage.input_tokens = static_cast<std::uint64_t>(prompt.size() / 3 + 32);
  resp.usage.output_tokens = static_cast<std::uint64_t>(answer.size() / 3 + 16);
  resp.blocks.push_back(core::TextContent{.text = std::move(answer)});
  if (sink != nullptr) {
    sink->on_done(resp.stop_reason);
  }
  co_return resp;
}

}  // namespace orangutan::personal
