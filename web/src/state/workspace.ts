import type {
  AgentGraph,
  AgentSummary,
  ApprovalRequest,
  ChatStreamEvent,
} from "../api/types";

export type MessageRole = "user" | "assistant";

export interface ToolCallRecord {
  id: string;
  name: string;
  input: unknown;
  /// `undefined` while streaming, `string` once the tool_end arrives.
  output?: string;
  isError?: boolean;
  startedAt: number;
}

export type StreamSegment =
  | { kind: "text"; text: string }
  | { kind: "thinking"; text: string }
  | { kind: "tool"; toolId: string };

export interface AssistantTurn {
  id: string;
  role: "assistant";
  segments: StreamSegment[];
  toolCalls: Map<string, ToolCallRecord>;
  streaming: boolean;
  error?: string;
}

export interface UserTurn {
  id: string;
  role: "user";
  text: string;
}

export type Turn = AssistantTurn | UserTurn;

export interface SessionState {
  id: string;
  agentKey: string;
  turns: Turn[];
  pendingApproval: ApprovalRequest | null;
  streaming: boolean;
  createdAt: number;
  activityAt: number;
  composer: string;
}

export interface WorkspaceState {
  agents: AgentSummary[];
  graph: AgentGraph | null;
  sessions: Map<string, SessionState>;
  focusAgent: string | null;
  focusSession: string | null;
  mode: "workspace" | "observatory";
  paletteOpen: boolean;
  connected: boolean;
}

export const initialWorkspace: WorkspaceState = {
  agents: [],
  graph: null,
  sessions: new Map(),
  focusAgent: null,
  focusSession: null,
  mode: "workspace",
  paletteOpen: false,
  connected: false,
};

export function makeEmptySession(id: string, agentKey: string): SessionState {
  return {
    id,
    agentKey,
    turns: [],
    pendingApproval: null,
    streaming: false,
    createdAt: Date.now(),
    activityAt: Date.now(),
    composer: "",
  };
}

export function applyStreamEvent(
  session: SessionState,
  assistantId: string,
  event: ChatStreamEvent,
): SessionState {
  const sessions = { ...session };
  sessions.activityAt = Date.now();
  const turns = session.turns.slice();
  const idx = turns.findIndex(
    (t) => t.role === "assistant" && t.id === assistantId,
  );
  if (idx === -1) return session;
  const prev = turns[idx] as AssistantTurn;
  const next: AssistantTurn = {
    ...prev,
    segments: prev.segments.slice(),
    toolCalls: new Map(prev.toolCalls),
  };

  switch (event.type) {
    case "session":
      break;
    case "text": {
      if (!event.text) break;
      const last = next.segments[next.segments.length - 1];
      if (last && last.kind === "text") {
        next.segments[next.segments.length - 1] = {
          kind: "text",
          text: last.text + event.text,
        };
      } else {
        next.segments.push({ kind: "text", text: event.text });
      }
      break;
    }
    case "thinking": {
      if (!event.thinking) break;
      const last = next.segments[next.segments.length - 1];
      if (last && last.kind === "thinking") {
        next.segments[next.segments.length - 1] = {
          kind: "thinking",
          text: last.text + event.thinking,
        };
      } else {
        next.segments.push({ kind: "thinking", text: event.thinking });
      }
      break;
    }
    case "tool_start": {
      const record: ToolCallRecord = {
        id: event.id,
        name: event.name,
        input: event.input,
        startedAt: Date.now(),
      };
      next.toolCalls.set(event.id, record);
      next.segments.push({ kind: "tool", toolId: event.id });
      break;
    }
    case "tool_end": {
      const existing = next.toolCalls.get(event.id);
      if (existing) {
        next.toolCalls.set(event.id, {
          ...existing,
          output: event.content,
          isError: event.is_error,
        });
      } else {
        next.toolCalls.set(event.id, {
          id: event.id,
          name: event.name,
          input: null,
          output: event.content,
          isError: event.is_error,
          startedAt: Date.now(),
        });
        next.segments.push({ kind: "tool", toolId: event.id });
      }
      break;
    }
    case "approval_request":
      sessions.pendingApproval = event.payload;
      break;
    case "done":
      next.streaming = false;
      sessions.streaming = false;
      sessions.pendingApproval = null;
      break;
    case "error":
      next.streaming = false;
      next.error = event.error;
      sessions.streaming = false;
      sessions.pendingApproval = null;
      break;
  }

  turns[idx] = next;
  sessions.turns = turns;
  return sessions;
}
