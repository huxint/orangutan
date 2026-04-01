#include "web/web-route-internal.hpp"

#include "types/serialization.hpp"

namespace orangutan::web {

    void handle_list_sessions(const httplib::Request & /*req*/, httplib::Response &res, storage::SessionStore *store) {
        if (store == nullptr) {
            res.status = 503;
            res.set_content(R"({"error":"session store not available"})", "application/json");
            return;
        }
        auto sessions = store->list_sessions();
        auto arr = nlohmann::json::array();
        for (const auto &session : sessions) {
            arr.push_back({
                {"id", session.id},
                {"created_at", session.created_at},
                {"model", session.model},
                {"message_count", session.message_count},
            });
        }
        res.set_content(arr.dump(), "application/json");
    }

    void handle_get_session(const httplib::Request &req, httplib::Response &res, storage::SessionStore *store) {
        if (store == nullptr) {
            res.status = 503;
            res.set_content(R"({"error":"session store not available"})", "application/json");
            return;
        }
        const auto session_id = std::string(req.matches[1]);
        std::vector<Message> messages;
        try {
            messages = store->load(session_id);
        } catch (const std::runtime_error &) {
            res.status = 404;
            res.set_content(R"({"error":"session not found"})", "application/json");
            return;
        }
        auto arr = nlohmann::json::array();
        for (const auto &message : messages) {
            arr.push_back(message_to_json(message));
        }
        const nlohmann::json body = {
            {"id", session_id},
            {"messages", arr},
        };
        res.set_content(body.dump(), "application/json");
    }

    void handle_delete_session(const httplib::Request &req, httplib::Response &res, storage::SessionStore *store) {
        if (store == nullptr) {
            res.status = 503;
            res.set_content(R"({"error":"session store not available"})", "application/json");
            return;
        }
        const auto session_id = std::string(req.matches[1]);
        store->remove(session_id);
        res.set_content(R"({"status":"deleted"})", "application/json");
    }

    void handle_list_agent_sessions(const httplib::Request &req, httplib::Response &res, config::Config *config, storage::SessionStore *store) {
        if (config == nullptr) {
            res.status = 503;
            res.set_content(R"({"error":"config not available"})", "application/json");
            return;
        }
        if (store == nullptr) {
            res.status = 503;
            res.set_content(R"({"error":"session store not available"})", "application/json");
            return;
        }

        const auto agent_key = std::string(req.matches[1]);
        if (!internal::find_effective_agent(config, agent_key).has_value()) {
            res.status = 404;
            res.set_content(R"({"error":"agent not found"})", "application/json");
            return;
        }

        auto arr = nlohmann::json::array();
        for (const auto &session : store->list_sessions_for_agent(agent_key)) {
            arr.push_back(internal::session_to_json(session));
        }
        res.set_content(arr.dump(), "application/json");
    }

    void handle_get_agent_session(const httplib::Request &req, httplib::Response &res, config::Config *config, storage::SessionStore *store) {
        if (config == nullptr) {
            res.status = 503;
            res.set_content(R"({"error":"config not available"})", "application/json");
            return;
        }
        if (store == nullptr) {
            res.status = 503;
            res.set_content(R"({"error":"session store not available"})", "application/json");
            return;
        }

        const auto agent_key = std::string(req.matches[1]);
        const auto session_id = std::string(req.matches[2]);
        if (!internal::find_effective_agent(config, agent_key).has_value()) {
            res.status = 404;
            res.set_content(R"({"error":"agent not found"})", "application/json");
            return;
        }

        const auto session = internal::find_agent_session(store, agent_key, session_id);
        if (!session.has_value()) {
            res.status = 404;
            res.set_content(R"({"error":"session not found"})", "application/json");
            return;
        }

        try {
            auto arr = nlohmann::json::array();
            for (const auto &message : store->load(session_id)) {
                arr.push_back(message_to_json(message));
            }

            auto body = internal::session_to_json(*session);
            body["messages"] = arr;
            res.set_content(body.dump(), "application/json");
        } catch (const std::runtime_error &) {
            res.status = 404;
            res.set_content(R"({"error":"session not found"})", "application/json");
        }
    }

    void handle_delete_agent_session(const httplib::Request &req, httplib::Response &res, config::Config *config, storage::SessionStore *store) {
        if (config == nullptr) {
            res.status = 503;
            res.set_content(R"({"error":"config not available"})", "application/json");
            return;
        }
        if (store == nullptr) {
            res.status = 503;
            res.set_content(R"({"error":"session store not available"})", "application/json");
            return;
        }

        const auto agent_key = std::string(req.matches[1]);
        const auto session_id = std::string(req.matches[2]);
        if (!internal::find_effective_agent(config, agent_key).has_value()) {
            res.status = 404;
            res.set_content(R"({"error":"agent not found"})", "application/json");
            return;
        }
        if (!store->session_belongs_to_agent(session_id, agent_key)) {
            res.status = 404;
            res.set_content(R"({"error":"session not found"})", "application/json");
            return;
        }

        store->remove(session_id);
        res.set_content(R"({"status":"deleted"})", "application/json");
    }

} // namespace orangutan::web
