// Types mirroring /api/v1 responses.

export interface ApiErrorBody {
  error: { code: string; message: string; details?: unknown };
}

export interface ServerInfo {
  name: string;
  api_version: string;
  capabilities: string[];
}

export interface AgentSummary {
  key: string;
  profile: string;
  model: string;
  workspace: string;
  edit_mode: string;
  team_agents: string[];
}

export interface AgentGraphNode {
  id: string;
  model: string;
  profile: string;
  workspace: string;
  edit_mode: string;
  coordinator_mode: boolean;
  team_size: number;
  live_sessions: number;
}

export interface AgentGraphEdge {
  source: string;
  target: string;
  kind: string;
}

export interface AgentGraph {
  nodes: AgentGraphNode[];
  edges: AgentGraphEdge[];
  generated_at: number;
}

export interface ToolDefinition {
  name: string;
  description: string;
  source: string;
}

export interface SkillSummary {
  id: string;
  name: string;
  description: string;
  tools: string[];
  source: string;
  scope: string;
  active: boolean;
  diagnostic_count: number;
  source_path: string;
}

export interface SystemStatus {
  uptime_seconds: number;
  active_web_sessions: number;
  provider_health: Record<string, unknown>;
  event_bus: { latest_sequence: number };
  automation: { automation_count: number } | null;
}

export interface ContentBlock {
  type: string;
  text?: string;
  thinking?: string;
  id?: string;
  name?: string;
  input?: unknown;
  tool_use_id?: string;
  content?: string;
  is_error?: boolean;
}

export interface SessionMessage {
  role: string;
  content: ContentBlock[];
}

export interface ChatSessionSummary {
  id: string;
  created_at: string;
  model: string;
  scope_key: string;
  agent_key: string;
  origin_kind: string;
  origin_ref: string;
  message_count: number;
  read_only: boolean;
  preview?: string;
}

export interface ChatSessionData extends ChatSessionSummary {
  messages: SessionMessage[];
}

export interface Page<T> {
  items: T[];
  next_cursor?: string | null;
  has_more?: boolean;
  total?: number;
}

export interface ApprovalRequest {
  request_id: string;
  tool: string;
  command?: string;
  sandbox_mode: string;
  prompt: string;
}

export type ChatStreamEvent =
  | { type: "session"; session_id: string }
  | { type: "text"; text: string }
  | { type: "thinking"; thinking: string }
  | { type: "tool_start"; id: string; name: string; input: unknown }
  | {
      type: "tool_end";
      id: string;
      name: string;
      content: string;
      is_error: boolean;
    }
  | { type: "approval_request"; payload: ApprovalRequest }
  | { type: "done" }
  | { type: "error"; error: string };

export interface BusEventEnvelope {
  kind: string;
  scope: string;
  payload: Record<string, unknown>;
  timestamp: number;
}
