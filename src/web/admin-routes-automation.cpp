#include "web/admin-routes-detail.hpp"
#include "web/web-routes.hpp"

#include "tools/automation/automation-tool-support.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <stdexcept>
#include <string>

namespace orangutan::web {
    namespace {

        using admin_detail::parse_optional_bool_param;
        using admin_detail::parse_request_json;
        using admin_detail::resolve_agent_key_or_default;
        using admin_detail::resolve_optional_agent_key;
        using admin_detail::set_json_error;

        [[nodiscard]]
        nlohmann::json unix_time_to_json(const std::optional<base::i64> &value) {
            if (!value.has_value()) {
                return nullptr;
            }
            return *value;
        }

        [[nodiscard]]
        nlohmann::json automation_to_json(const automation::Automation &automation) {
            return {
                {"id", automation.id},
                {"agent_key", automation.agent_key},
                {"name", automation.name},
                {"prompt", automation.prompt},
                {"notes", automation.notes},
                {"enabled", automation.enabled},
                {"paused", automation.paused},
                {"trigger", automation::trigger_to_json(automation.trigger)},
                {"delivery", automation::delivery_policy_to_json(automation.delivery)},
                {"tags", automation.tags},
                {"last_run_at", unix_time_to_json(automation.last_run_at)},
                {"next_due_at", unix_time_to_json(automation.next_due_at)},
                {"last_status", automation.last_status},
            };
        }

        [[nodiscard]]
        nlohmann::json run_to_json(const automation::RunRecord &run) {
            return {
                {"id", run.id},
                {"automation_id", run.automation_id},
                {"agent_key", run.agent_key},
                {"automation_name", run.automation_name},
                {"started_at", run.started_at},
                {"finished_at", unix_time_to_json(run.finished_at)},
                {"status", run.status},
                {"summary", run.summary},
                {"reply", run.reply},
                {"delivery_status", run.delivery_status},
                {"log_path", run.log_path},
            };
        }

        [[nodiscard]]
        nlohmann::json delivery_to_json(const automation::DeliveryRecord &delivery) {
            return {
                {"id", delivery.id},
                {"run_id", delivery.run_id},
                {"automation_id", delivery.automation_id},
                {"agent_key", delivery.agent_key},
                {"target", delivery.target},
                {"status", delivery.status},
                {"title", delivery.title},
                {"body", delivery.body},
                {"created_at", delivery.created_at},
                {"acked_at", unix_time_to_json(delivery.acked_at)},
            };
        }

        [[nodiscard]]
        std::expected<automation::AutomationQuery, std::string> parse_automation_query(const httplib::Request &req) {
            automation::AutomationQuery query;
            query.agent_key = resolve_optional_agent_key(req);

            const auto enabled = parse_optional_bool_param(req, "enabled");
            if (!enabled.has_value()) {
                return std::unexpected(enabled.error());
            }
            query.enabled = *enabled;

            const auto paused = parse_optional_bool_param(req, "paused");
            if (!paused.has_value()) {
                return std::unexpected(paused.error());
            }
            query.paused = *paused;
            return query;
        }

        [[nodiscard]]
        std::expected<automation::RunQuery, std::string> parse_run_query(const httplib::Request &req) {
            automation::RunQuery query;
            query.agent_key = resolve_optional_agent_key(req);
            if (req.has_param("automation_id")) {
                query.automation_id = req.get_param_value("automation_id");
            }
            return query;
        }

        [[nodiscard]]
        std::expected<automation::DeliveryQuery, std::string> parse_delivery_query(const httplib::Request &req, const nlohmann::json *body = nullptr) {
            automation::DeliveryQuery query;
            query.agent_key = resolve_optional_agent_key(req, body);
            if (req.has_param("automation_id")) {
                query.automation_id = req.get_param_value("automation_id");
            } else if (body != nullptr) {
                if (const auto it = body->find("automation_id"); it != body->end() && it->is_string()) {
                    query.automation_id = it->get<std::string>();
                }
            }
            if (req.has_param("run_id")) {
                query.run_id = req.get_param_value("run_id");
            } else if (body != nullptr) {
                if (const auto it = body->find("run_id"); it != body->end() && it->is_string()) {
                    query.run_id = it->get<std::string>();
                }
            }
            if (req.has_param("target")) {
                query.target = req.get_param_value("target");
            } else if (body != nullptr) {
                if (const auto it = body->find("target"); it != body->end() && it->is_string()) {
                    query.target = it->get<std::string>();
                }
            }

            const auto only_unacked = parse_optional_bool_param(req, "only_unacked");
            if (!only_unacked.has_value()) {
                return std::unexpected(only_unacked.error());
            }
            query.only_unacked = only_unacked->value_or(false);
            return query;
        }

    } // namespace

    void handle_list_automations(const httplib::Request &req, httplib::Response &res, automation::AutomationService *automation_service) {
        if (automation_service == nullptr) {
            set_json_error(res, 503, "automation service not available");
            return;
        }

        const auto query = parse_automation_query(req);
        if (!query.has_value()) {
            set_json_error(res, 400, query.error());
            return;
        }

        auto arr = nlohmann::json::array();
        for (const auto &automation : automation_service->list(*query)) {
            arr.push_back(automation_to_json(automation));
        }
        res.set_content(arr.dump(), "application/json");
    }

    void handle_create_automation(const httplib::Request &req, httplib::Response &res, automation::AutomationService *automation_service) {
        if (automation_service == nullptr) {
            set_json_error(res, 503, "automation service not available");
            return;
        }

        const auto body = parse_request_json(req);
        if (!body.has_value()) {
            set_json_error(res, 400, body.error());
            return;
        }

        const auto automation = builtin::detail::parse_create_request(*body, resolve_agent_key_or_default(req, &*body));
        if (!automation.has_value()) {
            set_json_error(res, 400, automation.error());
            return;
        }

        try {
            const auto id = automation_service->save(*automation);
            const auto stored = automation_service->find(automation->agent_key, id);
            res.status = 201;
            res.set_content((stored.has_value() ? automation_to_json(*stored) : nlohmann::json{{"id", id}}).dump(), "application/json");
        } catch (const std::exception &error) {
            set_json_error(res, 400, error.what());
        }
    }

    void handle_get_automation(const httplib::Request &req, httplib::Response &res, automation::AutomationService *automation_service) {
        if (automation_service == nullptr) {
            set_json_error(res, 503, "automation service not available");
            return;
        }

        const auto automation = automation_service->find(resolve_agent_key_or_default(req), std::string(req.matches[1]));
        if (!automation.has_value()) {
            set_json_error(res, 404, "automation not found");
            return;
        }
        res.set_content(automation_to_json(*automation).dump(), "application/json");
    }

    void handle_patch_automation(const httplib::Request &req, httplib::Response &res, automation::AutomationService *automation_service) {
        if (automation_service == nullptr) {
            set_json_error(res, 503, "automation service not available");
            return;
        }

        const auto body = parse_request_json(req);
        if (!body.has_value()) {
            set_json_error(res, 400, body.error());
            return;
        }

        const auto agent_key = resolve_agent_key_or_default(req, &*body);
        const auto existing = automation_service->find(agent_key, std::string(req.matches[1]));
        if (!existing.has_value()) {
            set_json_error(res, 404, "automation not found");
            return;
        }

        const auto updated = builtin::detail::apply_update_request(*body, *existing, agent_key);
        if (!updated.has_value()) {
            set_json_error(res, 400, updated.error());
            return;
        }

        try {
            const auto id = automation_service->save(*updated);
            const auto stored = automation_service->find(agent_key, id);
            res.set_content((stored.has_value() ? automation_to_json(*stored) : nlohmann::json{{"id", id}}).dump(), "application/json");
        } catch (const std::exception &error) {
            set_json_error(res, 400, error.what());
        }
    }

    void handle_delete_automation(const httplib::Request &req, httplib::Response &res, automation::AutomationService *automation_service) {
        if (automation_service == nullptr) {
            set_json_error(res, 503, "automation service not available");
            return;
        }

        if (!automation_service->remove(resolve_agent_key_or_default(req), std::string(req.matches[1]))) {
            set_json_error(res, 404, "automation not found");
            return;
        }
        res.set_content(R"({"status":"deleted"})", "application/json");
    }

    void handle_run_automation(const httplib::Request &req, httplib::Response &res, automation::AutomationService *automation_service) {
        if (automation_service == nullptr) {
            set_json_error(res, 503, "automation service not available");
            return;
        }

        try {
            const auto run_id = automation_service->run_now(resolve_agent_key_or_default(req), std::string(req.matches[1]));
            res.set_content(nlohmann::json{{"run_id", run_id}}.dump(), "application/json");
        } catch (const std::exception &error) {
            set_json_error(res, std::string_view(error.what()) == "automation not found" ? 404 : 400, error.what());
        }
    }

    void handle_pause_automation(const httplib::Request &req, httplib::Response &res, automation::AutomationService *automation_service) {
        if (automation_service == nullptr) {
            set_json_error(res, 503, "automation service not available");
            return;
        }

        if (!automation_service->pause(resolve_agent_key_or_default(req), std::string(req.matches[1]))) {
            set_json_error(res, 404, "automation not found");
            return;
        }
        res.set_content(R"({"status":"paused"})", "application/json");
    }

    void handle_resume_automation(const httplib::Request &req, httplib::Response &res, automation::AutomationService *automation_service) {
        if (automation_service == nullptr) {
            set_json_error(res, 503, "automation service not available");
            return;
        }

        if (!automation_service->resume(resolve_agent_key_or_default(req), std::string(req.matches[1]))) {
            set_json_error(res, 404, "automation not found");
            return;
        }
        res.set_content(R"({"status":"resumed"})", "application/json");
    }

    void handle_list_automation_runs(const httplib::Request &req, httplib::Response &res, automation::AutomationService *automation_service) {
        if (automation_service == nullptr) {
            set_json_error(res, 503, "automation service not available");
            return;
        }

        const auto query = parse_run_query(req);
        if (!query.has_value()) {
            set_json_error(res, 400, query.error());
            return;
        }

        auto arr = nlohmann::json::array();
        for (const auto &run : automation_service->list_runs(*query)) {
            arr.push_back(run_to_json(run));
        }
        res.set_content(arr.dump(), "application/json");
    }

    void handle_list_automation_deliveries(const httplib::Request &req, httplib::Response &res, automation::AutomationService *automation_service) {
        if (automation_service == nullptr) {
            set_json_error(res, 503, "automation service not available");
            return;
        }

        const auto query = parse_delivery_query(req);
        if (!query.has_value()) {
            set_json_error(res, 400, query.error());
            return;
        }

        auto arr = nlohmann::json::array();
        for (const auto &delivery : automation_service->list_deliveries(*query)) {
            arr.push_back(delivery_to_json(delivery));
        }
        res.set_content(arr.dump(), "application/json");
    }

    void handle_ack_automation_delivery(const httplib::Request &req, httplib::Response &res, automation::AutomationService *automation_service) {
        if (automation_service == nullptr) {
            set_json_error(res, 503, "automation service not available");
            return;
        }

        std::optional<nlohmann::json> body;
        if (!req.body.empty()) {
            const auto parsed = parse_request_json(req);
            if (!parsed.has_value()) {
                set_json_error(res, 400, parsed.error());
                return;
            }
            body = *parsed;
        }

        const auto agent_key = resolve_optional_agent_key(req, body ? &*body : nullptr);
        if (agent_key.empty()) {
            set_json_error(res, 400, "agent_key is required");
            return;
        }

        if (!automation_service->ack_delivery(agent_key, std::string(req.matches[1]))) {
            set_json_error(res, 404, "delivery not found");
            return;
        }
        res.set_content(R"({"status":"acknowledged"})", "application/json");
    }

    void handle_clear_automation_deliveries(const httplib::Request &req, httplib::Response &res, automation::AutomationService *automation_service) {
        if (automation_service == nullptr) {
            set_json_error(res, 503, "automation service not available");
            return;
        }

        std::optional<nlohmann::json> body;
        if (!req.body.empty()) {
            const auto parsed = parse_request_json(req);
            if (!parsed.has_value()) {
                set_json_error(res, 400, parsed.error());
                return;
            }
            body = *parsed;
        }

        const auto query = parse_delivery_query(req, body ? &*body : nullptr);
        if (!query.has_value()) {
            set_json_error(res, 400, query.error());
            return;
        }
        if (query->agent_key.empty()) {
            set_json_error(res, 400, "agent_key is required");
            return;
        }

        try {
            automation_service->clear_deliveries(*query);
            res.set_content(R"({"status":"cleared"})", "application/json");
        } catch (const std::exception &error) {
            set_json_error(res, 400, error.what());
        }
    }

} // namespace orangutan::web
