import type {
  AgentGraph,
  AgentSummary,
  ApiErrorBody,
  ApprovalRequest,
  ChatSessionData,
  ChatSessionSummary,
  ChatStreamEvent,
  Page,
  ServerInfo,
  SkillSummary,
  SystemStatus,
  ToolDefinition,
} from "./types";

const BASE = "/api/v1";

export class ApiError extends Error {
  constructor(
    public readonly status: number,
    public readonly code: string,
    message: string,
    public readonly details?: unknown,
  ) {
    super(message);
    this.name = "ApiError";
  }
}

async function toApiError(res: Response): Promise<ApiError> {
  const text = await res.text();
  if (!text) return new ApiError(res.status, "http", `HTTP ${res.status}`);
  try {
    const body = JSON.parse(text) as ApiErrorBody | { error?: string };
    if (body && typeof body === "object" && "error" in body) {
      const err = body.error;
      if (typeof err === "string")
        return new ApiError(res.status, "legacy", err);
      if (err && typeof err === "object")
        return new ApiError(
          res.status,
          err.code ?? "error",
          err.message ?? "error",
          err.details,
        );
    }
  } catch {
    /* fall through */
  }
  return new ApiError(res.status, "http", text);
}

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  const res = await fetch(`${BASE}${path}`, {
    cache: "no-store",
    ...init,
    headers: { "Content-Type": "application/json", ...init?.headers },
  });
  if (!res.ok) throw await toApiError(res);
  if (res.status === 204) return undefined as T;
  return (await res.json()) as T;
}

export const api = {
  server: () => request<ServerInfo>("/server"),
  agents: () => request<{ items: AgentSummary[] }>("/agents"),
  graph: () => request<AgentGraph>("/agents/graph"),
  tools: () => request<{ items: ToolDefinition[] }>("/tools"),
  skills: () => request<{ items: SkillSummary[] }>("/skills"),
  system: () => request<SystemStatus>("/system"),
  agentSessions: (key: string) =>
    request<Page<ChatSessionSummary>>(
      `/agents/${encodeURIComponent(key)}/sessions`,
    ),
  agentSession: (key: string, sessionId: string) =>
    request<ChatSessionData>(
      `/agents/${encodeURIComponent(key)}/sessions/${encodeURIComponent(sessionId)}`,
    ),
  deleteAgentSession: (key: string, sessionId: string) =>
    request<{ status: string; id: string }>(
      `/agents/${encodeURIComponent(key)}/sessions/${encodeURIComponent(sessionId)}`,
      { method: "DELETE" },
    ),
  submitApproval: (
    sessionId: string,
    requestId: string,
    approved: boolean,
  ): Promise<{ status: "approved" | "denied" }> =>
    request("/chat/approval", {
      method: "POST",
      body: JSON.stringify({
        session_id: sessionId,
        request_id: requestId,
        approved,
      }),
    }),
  abortSession: (sessionId: string) =>
    request<{ status: string }>("/chat/abort", {
      method: "POST",
      body: JSON.stringify({ session_id: sessionId }),
    }),
};

export interface StreamChatOptions {
  agentKey: string;
  sessionId?: string | null;
  message: string;
  onEvent: (event: ChatStreamEvent) => void;
  signal?: AbortSignal;
}

export async function streamChat({
  agentKey,
  sessionId,
  message,
  onEvent,
  signal,
}: StreamChatOptions): Promise<void> {
  const body: Record<string, string> = { agent_key: agentKey, message };
  if (sessionId) body.session_id = sessionId;
  const res = await fetch(`${BASE}/chat`, {
    method: "POST",
    cache: "no-store",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
    signal,
  });
  if (!res.ok) throw await toApiError(res);
  if (!res.body) throw new Error("response body missing");

  const reader = res.body.getReader();
  const decoder = new TextDecoder();
  let buffer = "";

  const dispatch = (eventType: string, raw: string) => {
    try {
      const data = JSON.parse(raw) as Record<string, unknown>;
      onEvent(parseChatEvent(eventType, data));
    } catch {
      /* swallow malformed line */
    }
  };

  for (;;) {
    const { value, done } = await reader.read();
    if (done) return;
    buffer += decoder.decode(value, { stream: true });
    let idx = buffer.indexOf("\n\n");
    while (idx !== -1) {
      const frame = buffer.slice(0, idx);
      buffer = buffer.slice(idx + 2);
      const lines = frame.split("\n");
      let ev = "";
      let data = "";
      for (const line of lines) {
        if (line.startsWith("event: ")) ev = line.slice(7);
        else if (line.startsWith("data: "))
          data = data ? `${data}\n${line.slice(6)}` : line.slice(6);
      }
      if (ev && data) dispatch(ev, data);
      idx = buffer.indexOf("\n\n");
    }
  }
}

function parseChatEvent(
  type: string,
  data: Record<string, unknown>,
): ChatStreamEvent {
  switch (type) {
    case "session":
      return { type: "session", session_id: String(data.session_id ?? "") };
    case "text":
      return { type: "text", text: String(data.text ?? "") };
    case "thinking":
      return { type: "thinking", thinking: String(data.thinking ?? "") };
    case "tool_start":
      return {
        type: "tool_start",
        id: String(data.id ?? ""),
        name: String(data.name ?? ""),
        input: data.input,
      };
    case "tool_end":
      return {
        type: "tool_end",
        id: String(data.id ?? ""),
        name: String(data.name ?? ""),
        content: String(data.content ?? ""),
        is_error: Boolean(data.is_error),
      };
    case "approval_request":
      return {
        type: "approval_request",
        payload: data as unknown as ApprovalRequest,
      };
    case "done":
      return { type: "done" };
    case "error":
      return { type: "error", error: String(data.error ?? "unknown error") };
    default:
      return { type: "error", error: `unknown event: ${type}` };
  }
}
