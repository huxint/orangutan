export interface ServerHealth {
  ok: boolean;
  runtime: string;
  mode: 'demo' | 'live';
  workspace: string;
  model: string;
}

export interface ServerStateSnapshot {
  mode: 'demo' | 'live';
  workspace: string;
  model: string;
  permission: 'ask' | 'allow' | 'deny';
  conversations: Array<{
    id: string;
    sessionId: string;
    name: string;
    customName: boolean;
    createdAt: number;
  }>;
  messages: Array<{
    id: string;
    conversationId: string;
    sessionId: string;
    role: 'user' | 'assistant';
    text: string;
    createdAt: number;
    state: 'complete' | 'streaming' | 'interrupted';
  }>;
  tasks: Array<{
    id: string;
    conversationId: string;
    sessionId: string;
    title: string;
    status: string;
    activeCompanion: string;
    activeTool: string;
    createdAt: number;
    approval: { id: string; status: string; target: string; tool: string } | null;
  }>;
  notes: Array<{
    id: string;
    scope: 'shared' | 'personal';
    ownerId: string | null;
    title: string;
    body: string;
    source: string;
    updatedAt: number;
  }>;
  reminders: Array<{
    id: string;
    title: string;
    dueAt: number;
    completed: boolean;
    notified?: boolean;
    createdAt: number;
  }>;
}

async function apiJson<T>(path: string, init?: RequestInit): Promise<T | null> {
  try {
    const res = await fetch(path, {
      ...init,
      headers: { 'Content-Type': 'application/json', ...(init?.headers ?? {}) },
    });
    if (!res.ok) return null;
    return (await res.json()) as T;
  } catch {
    return null;
  }
}

export async function probePersonalServer(): Promise<ServerHealth | null> {
  const controller = new AbortController();
  const timer = window.setTimeout(() => controller.abort(), 500);
  try {
    const res = await fetch('/api/health', { signal: controller.signal });
    if (!res.ok) return null;
    const data = (await res.json()) as Partial<ServerHealth>;
    if (data.ok && data.runtime === 'orangutan-cpp26') return data as ServerHealth;
    return null;
  } catch {
    return null;
  } finally {
    window.clearTimeout(timer);
  }
}

export const fetchServerState = () => apiJson<ServerStateSnapshot>('/api/state');

export const createServerConversation = (name?: string) =>
  apiJson<{ id: string; sessionId: string; name: string; createdAt: number }>(
    '/api/conversations',
    { method: 'POST', body: JSON.stringify(name ? { name } : {}) },
  );

export const renameServerConversation = (id: string, name: string) =>
  apiJson(`/api/conversations/${encodeURIComponent(id)}`, {
    method: 'PATCH',
    body: JSON.stringify({ name }),
  });

export const sendServerMessage = (conversationId: string, text: string) =>
  apiJson<{ taskId: string; conversationId: string; sessionId: string; status: string }>(
    `/api/conversations/${encodeURIComponent(conversationId)}/messages`,
    { method: 'POST', body: JSON.stringify({ text }) },
  );

export const stopServerTask = (taskId: string) =>
  apiJson(`/api/tasks/${encodeURIComponent(taskId)}/stop`, { method: 'POST' });

export const resolveServerApproval = (approvalId: string, decision: 'approve' | 'reject') =>
  apiJson(`/api/approvals/${encodeURIComponent(approvalId)}`, {
    method: 'POST',
    body: JSON.stringify({ decision }),
  });

export const upsertServerMemory = (note: {
  id?: string;
  scope: 'shared' | 'personal';
  title: string;
  body: string;
}) =>
  apiJson(note.id ? `/api/memory/${encodeURIComponent(note.id)}` : '/api/memory', {
    method: note.id ? 'PUT' : 'POST',
    body: JSON.stringify(note),
  });

export const deleteServerMemory = (id: string, scope: 'shared' | 'personal') =>
  apiJson(`/api/memory/${encodeURIComponent(id)}?scope=${scope}`, { method: 'DELETE' });

export const addServerReminder = (title: string, dueAt: number) =>
  apiJson<{ id: string; title: string; dueAt: number; completed: boolean; createdAt: number }>(
    '/api/reminders',
    { method: 'POST', body: JSON.stringify({ title, dueAt }) },
  );

export const toggleServerReminder = (id: string, completed: boolean) =>
  apiJson(`/api/reminders/${encodeURIComponent(id)}/done`, {
    method: 'POST',
    body: JSON.stringify({ completed }),
  });

export const deleteServerReminder = (id: string) =>
  apiJson(`/api/reminders/${encodeURIComponent(id)}`, { method: 'DELETE' });

export const updateServerSettings = (permission: 'ask' | 'allow' | 'deny') =>
  apiJson('/api/settings', { method: 'PATCH', body: JSON.stringify({ permission }) });
