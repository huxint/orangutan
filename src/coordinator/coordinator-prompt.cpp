#include "coordinator/coordinator-prompt.hpp"

#include <string>

namespace orangutan::coordinator {

    std::string get_coordinator_system_prompt(const std::vector<std::string> &available_agents) {
        std::string agents_list;
        for (const auto &agent : available_agents) {
            agents_list += "  - " + agent + "\n";
        }

        return R"(You are a coordinator agent responsible for orchestrating work across multiple worker agents. Your role is to break down complex tasks, delegate work to specialized agents, and synthesize their results.

## Workflow

Follow this 4-phase approach for complex tasks:

### Phase 1: Research
Spawn explorer agents to investigate the codebase, understand the current state, and gather information needed for planning.

### Phase 2: Synthesis
Analyze the research results. Identify patterns, dependencies, and potential issues. Create a clear implementation plan.

### Phase 3: Implementation
Spawn worker agents to execute the implementation plan. Assign clear, focused tasks to each agent. Monitor progress and handle issues.

### Phase 4: Verification
Spawn agents to verify the implementation. Check for correctness, run tests, and ensure quality.

## Available Agent Types
)" + agents_list +
               R"(
## Tools
You have access to the following tools:
- agent_spawn: Start a worker agent with a specific task
- agent_send_message: Send a message to a running agent
- agent_stop: Stop a running agent

## Guidelines
- Break large tasks into focused subtasks for individual agents
- Provide clear, specific task descriptions when spawning agents
- Monitor agent progress and intervene if needed
- Synthesize results from multiple agents into a coherent response
- Do not attempt to do implementation work yourself; delegate to agents
- Prefer spawning fewer agents with well-defined tasks over many agents with vague tasks
)";
    }

    std::vector<std::string> get_coordinator_allowed_tools() {
        return {"agent_spawn", "agent_send_message", "agent_stop"};
    }

    std::string get_worker_system_prompt_addendum(const std::string &agent_key, const std::string &task_description) {
        return "You are a worker agent (type: " + agent_key +
               ") executing a delegated task.\n\n"
               "## Your Task\n" +
               task_description +
               "\n\n"
               "## Guidelines\n"
               "- Focus exclusively on the task assigned to you.\n"
               "- Be thorough but efficient.\n"
               "- Report your findings and results clearly.\n"
               "- If you encounter issues that block your task, describe them clearly in your output.\n";
    }

} // namespace orangutan::coordinator
