import { Bot } from "lucide-react";
import { cn } from "../../lib/utils";
import type { AgentSummary } from "../../api/client";

interface AgentTreeProps {
  agents: AgentSummary[];
  currentAgentKey?: string;
  onSelectAgent: (agentKey: string) => void;
}

function buildTree(agents: AgentSummary[]) {
  const agentMap = new Map(agents.map((agent) => [agent.key, agent]));
  const referencedChildren = new Set<string>();

  for (const agent of agents) {
    for (const childKey of agent.team_agents ?? []) {
      if (agentMap.has(childKey)) {
        referencedChildren.add(childKey);
      }
    }
  }

  return agents
    .filter((agent) => !referencedChildren.has(agent.key))
    .sort((a, b) => a.key.localeCompare(b.key))
    .map((agent) => ({
      agent,
      children: (agent.team_agents ?? [])
        .map((childKey) => agentMap.get(childKey))
        .filter((child): child is AgentSummary => Boolean(child))
        .sort((a, b) => a.key.localeCompare(b.key)),
    }));
}

function renderBranch(
  agent: AgentSummary,
  currentAgentKey: string | undefined,
  onSelectAgent: (agentKey: string) => void,
  childMap: Map<string, AgentSummary[]>,
  depth = 0,
) {
  const children = childMap.get(agent.key) ?? [];
  const isActive = currentAgentKey === agent.key;

  return (
    <div
      key={agent.key}
      className="animate-fade-in"
      style={{ animationDelay: `${depth * 50}ms` }}
    >
      <button
        type="button"
        onClick={() => onSelectAgent(agent.key)}
        className={cn(
          "w-full rounded-xl px-3 py-2 text-left transition-all duration-200 group",
          isActive
            ? "bg-accent-bg text-accent shadow-sm"
            : "text-text-muted hover:bg-bg-elevated hover:text-text",
        )}
        style={{ paddingLeft: `${12 + depth * 16}px` }}
      >
        <div className="flex items-center gap-2">
          <div
            className={cn(
              "p-1 rounded-lg transition-colors",
              isActive
                ? "bg-accent/15"
                : "bg-bg-elevated group-hover:bg-accent/10",
            )}
          >
            <Bot size={12} />
          </div>
          <span className="text-sm font-medium truncate">{agent.key}</span>
        </div>
        <div
          className="mt-0.5 truncate text-[11px] text-text-muted"
          style={{ paddingLeft: 28 }}
        >
          {agent.model}
        </div>
      </button>
      {children.map((child) =>
        renderBranch(
          child,
          currentAgentKey,
          onSelectAgent,
          childMap,
          depth + 1,
        ),
      )}
    </div>
  );
}

export function AgentTree({
  agents,
  currentAgentKey,
  onSelectAgent,
}: AgentTreeProps) {
  const roots = buildTree(agents);
  const childMap = new Map<string, AgentSummary[]>();

  for (const agent of agents) {
    childMap.set(
      agent.key,
      (agent.team_agents ?? [])
        .map((childKey) =>
          agents.find((candidate) => candidate.key === childKey),
        )
        .filter((child): child is AgentSummary => Boolean(child))
        .sort((a, b) => a.key.localeCompare(b.key)),
    );
  }

  return (
    <div className="space-y-0.5">
      {roots.map((root) =>
        renderBranch(root.agent, currentAgentKey, onSelectAgent, childMap),
      )}
    </div>
  );
}
