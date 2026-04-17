#include "web/errors.hpp"
#include "web/pagination.hpp"
#include "web/web-route-internal.hpp"

#include "types/serialization.hpp"

namespace orangutan::web {

    namespace {

        nlohmann::json flat_session_summary(const storage::SessionInfo &session) {
            return {
                {"id", session.id},
                {"created_at", session.created_at},
                {"model", session.model},
                {"message_count", session.message_count},
            };
        }

    } // namespace

    void handle_list_sessions(const WebContext &ctx, const httplib::Request &req, httplib::Response &res) {
        if (ctx.session_store == nullptr) {
            send_error(res, 503, "store_unavailable", "session store not available");
            return;
        }
        const auto sessions = ctx.session_store->list_sessions();
        const auto page = parse_page(req);
        send_json(res, paginate(std::span<const storage::SessionInfo>(sessions), page, flat_session_summary));
    }

    void handle_get_session(const WebContext &ctx, const httplib::Request &req, httplib::Response &res) {
        if (ctx.session_store == nullptr) {
            send_error(res, 503, "store_unavailable", "session store not available");
            return;
        }
        const auto session_id = std::string(req.matches[1]);
        std::vector<Message> messages;
        try {
            messages = ctx.session_store->load(session_id);
        } catch (const std::runtime_error &) {
            send_error(res, 404, "session_not_found", "session not found");
            return;
        }
        auto arr = nlohmann::json::array();
        for (const auto &message : messages) {
            arr.push_back(message_to_json(message));
        }
        send_json(res, {{"id", session_id}, {"messages", arr}});
    }

    void handle_delete_session(const WebContext &ctx, const httplib::Request &req, httplib::Response &res) {
        if (ctx.session_store == nullptr) {
            send_error(res, 503, "store_unavailable", "session store not available");
            return;
        }
        const auto session_id = std::string(req.matches[1]);
        ctx.session_store->remove(session_id);
        send_json(res, {{"status", "deleted"}, {"id", session_id}});
    }

    void handle_list_agent_sessions(const WebContext &ctx, const httplib::Request &req, httplib::Response &res) {
        if (ctx.config == nullptr) {
            send_error(res, 503, "config_unavailable", "config not available");
            return;
        }
        if (ctx.session_store == nullptr) {
            send_error(res, 503, "store_unavailable", "session store not available");
            return;
        }

        const auto agent_key = std::string(req.matches[1]);
        if (!internal::find_effective_agent(ctx.config, agent_key).has_value()) {
            send_error(res, 404, "agent_not_found", "agent not found");
            return;
        }

        const auto sessions = ctx.session_store->list_sessions_for_agent(agent_key);
        const auto page = parse_page(req);
        send_json(res, paginate(std::span<const storage::SessionInfo>(sessions), page, internal::session_to_json));
    }

    void handle_get_agent_session(const WebContext &ctx, const httplib::Request &req, httplib::Response &res) {
        if (ctx.config == nullptr) {
            send_error(res, 503, "config_unavailable", "config not available");
            return;
        }
        if (ctx.session_store == nullptr) {
            send_error(res, 503, "store_unavailable", "session store not available");
            return;
        }

        const auto agent_key = std::string(req.matches[1]);
        const auto session_id = std::string(req.matches[2]);
        if (!internal::find_effective_agent(ctx.config, agent_key).has_value()) {
            send_error(res, 404, "agent_not_found", "agent not found");
            return;
        }

        const auto session = internal::find_agent_session(ctx.session_store, agent_key, session_id);
        if (!session.has_value()) {
            send_error(res, 404, "session_not_found", "session not found");
            return;
        }

        try {
            auto arr = nlohmann::json::array();
            for (const auto &message : ctx.session_store->load(session_id)) {
                arr.push_back(message_to_json(message));
            }
            auto body = internal::session_to_json(*session);
            body["messages"] = arr;
            send_json(res, body);
        } catch (const std::runtime_error &) {
            send_error(res, 404, "session_not_found", "session not found");
        }
    }

    void handle_delete_agent_session(const WebContext &ctx, const httplib::Request &req, httplib::Response &res) {
        if (ctx.config == nullptr) {
            send_error(res, 503, "config_unavailable", "config not available");
            return;
        }
        if (ctx.session_store == nullptr) {
            send_error(res, 503, "store_unavailable", "session store not available");
            return;
        }

        const auto agent_key = std::string(req.matches[1]);
        const auto session_id = std::string(req.matches[2]);
        if (!internal::find_effective_agent(ctx.config, agent_key).has_value()) {
            send_error(res, 404, "agent_not_found", "agent not found");
            return;
        }
        if (!ctx.session_store->session_belongs_to_agent(session_id, agent_key)) {
            send_error(res, 404, "session_not_found", "session not found");
            return;
        }

        ctx.session_store->remove(session_id);
        send_json(res, {{"status", "deleted"}, {"id", session_id}, {"agent_key", agent_key}});
    }

} // namespace orangutan::web
