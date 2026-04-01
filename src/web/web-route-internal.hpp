#pragma once

#include "automation/scheduler.hpp"
#include "cli/slash-commands.hpp"
#include "config/config.hpp"
#include "storage/session-store.hpp"
#include "web/web-routes.hpp"
#include "web/web-types.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace orangutan::web::internal {

    [[nodiscard]]
    bool session_is_read_only(const storage::SessionInfo &session);

    [[nodiscard]]
    storage::SessionMetadata make_web_session_metadata(const std::string &agent_key, const config::AgentConfig &agent);

    [[nodiscard]]
    std::optional<config::AgentConfig> find_effective_agent(const config::Config *config, const std::string &agent_key);

    [[nodiscard]]
    nlohmann::json session_to_json(const storage::SessionInfo &session);

    [[nodiscard]]
    std::optional<storage::SessionInfo> find_agent_session(storage::SessionStore *store, const std::string &agent_key, const std::string &session_id);

    [[nodiscard]]
    std::string resolve_agent_key_param(const httplib::Request &req);

    [[nodiscard]]
    nlohmann::json task_to_json(const automation::TaskSpec &task);

    [[nodiscard]]
    nlohmann::json heartbeat_to_json(const automation::HeartbeatSpec &heartbeat);

    [[nodiscard]]
    nlohmann::json inbox_item_to_json(const automation::InboxItem &item);

    void write_sse_event(httplib::DataSink &sink, std::string_view event_name, const nlohmann::json &payload);

    void send_web_command_stream(httplib::Response &res, const cli::SlashCommandReply &command_response);

    [[nodiscard]]
    cli::SlashCommandReply handle_web_static_slash_command(const std::string &message, const std::string &agent_key, const config::Config &config, storage::SessionStore *store,
                                                           const std::optional<storage::SessionInfo> &existing_session, const storage::SessionMetadata &metadata,
                                                           const std::string &current_session_id);

    [[nodiscard]]
    cli::SlashCommandReply handle_web_runtime_slash_command(const std::string &message, const std::string &agent_key, const config::AgentConfig &agent,
                                                            storage::SessionStore *store, const storage::SessionMetadata &metadata, bootstrap::AgentRuntimeBundle &runtime,
                                                            std::string &current_session_id);

} // namespace orangutan::web::internal
