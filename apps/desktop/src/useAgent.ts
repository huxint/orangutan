import { useEffect, useRef, useState } from 'react';
import { active, advanceDemo, command, recover, settleConnection, type Command } from './model.ts';
import { loadState, resetState, saveState, STORAGE_KEY, type StoragePort } from './storage.ts';
import { conversationKey, requesterKey, type AppState, type Status } from './schema.ts';
import type { RuntimeInfo } from './context.ts';
import type { PetId } from './pets.tsx';
import {
  addServerReminder,
  createServerConversation,
  deleteServerMemory,
  deleteServerReminder,
  fetchServerState,
  probePersonalServer,
  renameServerConversation,
  resolveServerApproval,
  sendServerMessage,
  stopServerTask,
  toggleServerReminder,
  updateServerSettings,
  upsertServerMemory,
  type ServerStateSnapshot,
} from './runtime.ts';

const browserStorage: StoragePort = {
  getItem: (key) => window.localStorage.getItem(key),
  setItem: (key, value) => window.localStorage.setItem(key, value),
};

function mergeServerSnapshot(previous: AppState, snap: ServerStateSnapshot): AppState {
  const localConvs = snap.conversations.map((c) => ({
    id: conversationKey('desktop', c.id),
    connectionId: 'desktop',
    locator: c.id,
    requesterId: requesterKey('desktop', 'you'),
    sessionId: c.sessionId,
    name: c.name,
    customName: c.customName,
    origin: 'desktop' as const,
    unread: 0,
  }));
  const channelConvs = previous.conversations.filter((c) => c.origin === 'channel');
  const conversations = [...localConvs, ...channelConvs];
  const selectedId = conversations.some((c) => c.id === previous.selectedId)
    ? previous.selectedId
    : (localConvs[0]?.id ?? previous.selectedId);

  const localMessages = snap.messages.map((m) => ({
    id: m.id,
    conversationId: conversationKey('desktop', m.conversationId),
    sessionId: m.sessionId,
    role: m.role,
    text: m.text,
    createdAt: m.createdAt,
    state: m.state,
  }));
  const channelMessages = previous.messages.filter((m) =>
    channelConvs.some((c) => c.id === m.conversationId),
  );

  const validStatuses = new Set<Status>([
    'queued',
    'running',
    'awaiting_approval',
    'cancelling',
    'completed',
    'failed',
    'cancelled',
    'interrupted',
  ]);
  const localTasks = snap.tasks.map((t) => ({
    id: t.id,
    conversationId: conversationKey('desktop', t.conversationId),
    requesterId: requesterKey('desktop', 'you'),
    sessionId: t.sessionId,
    title: t.title,
    kind: 'chat' as const,
    status: (validStatuses.has(t.status as Status) ? t.status : 'running') as Status,
    attempt: 1,
    step: 0,
    createdAt: t.createdAt,
    language: previous.settings.language,
    shouldFail: false,
    history: [],
    members: [],
    approval: t.approval
      ? {
          id: t.approval.id,
          status: t.approval.status as
            'pending' | 'approved' | 'rejected' | 'expired' | 'cancelled',
          target: `${t.approval.tool}: ${t.approval.target}`,
        }
      : null,
    artifact: null,
  }));
  const channelTasks = previous.tasks.filter((t) =>
    channelConvs.some((c) => c.id === t.conversationId),
  );

  return {
    ...previous,
    settings: { ...previous.settings, permission: snap.permission },
    selectedId,
    conversations,
    messages: [...localMessages, ...channelMessages],
    tasks: [...localTasks, ...channelTasks],
    notes: snap.notes,
    reminders: snap.reminders,
  };
}

export function useAgent() {
  const [loaded] = useState(() => loadState(browserStorage));
  const [state, setState] = useState(loaded.state);
  const [storageError, setStorageError] = useState(loaded.error);
  const [runtime, setRuntime] = useState<RuntimeInfo>({ mode: 'offline' });
  const blocked = useRef(loaded.blocked);
  const liveRef = useRef(false);

  useEffect(() => {
    if (!liveRef.current && !blocked.current)
      setStorageError(saveState(browserStorage, state) ? null : 'write');
  }, [state]);

  // Probe local C++26 Personal Agent host and bind SSE event stream if available.
  useEffect(() => {
    let disposed = false;
    let source: EventSource | null = null;

    const refresh = async () => {
      const snap = await fetchServerState();
      if (!disposed && snap) {
        setState((prev) => mergeServerSnapshot(prev, snap));
      }
    };

    void probePersonalServer().then(async (health) => {
      if (disposed || !health) return;
      liveRef.current = true;
      setRuntime({
        mode: health.mode === 'live' ? 'live' : 'live-demo',
        workspace: health.workspace,
        model: health.model,
      });
      await refresh();
      if (disposed) return;

      source = new EventSource('/api/events');
      source.addEventListener('text_delta', (event) => {
        const data = JSON.parse((event as MessageEvent<string>).data) as {
          taskId: string;
          conversationId: string;
          delta: string;
        };
        const cid = conversationKey('desktop', data.conversationId);
        const msgId = `${data.taskId}:live:reply`;
        setState((prev) => {
          const exists = prev.messages.some((m) => m.id === msgId);
          return {
            ...prev,
            messages: exists
              ? prev.messages.map((m) =>
                  m.id === msgId ? { ...m, text: m.text + data.delta, state: 'streaming' } : m,
                )
              : [
                  ...prev.messages,
                  {
                    id: msgId,
                    conversationId: cid,
                    sessionId: data.conversationId,
                    role: 'assistant',
                    text: data.delta,
                    createdAt: Date.now(),
                    state: 'streaming',
                  },
                ],
          };
        });
      });

      source.addEventListener('tool_activity', (event) => {
        const data = JSON.parse((event as MessageEvent<string>).data) as {
          phase: 'start' | 'end';
          tool: string;
          companion: PetId;
        };
        setRuntime((prev) => ({
          ...prev,
          activeCompanion: data.phase === 'start' ? data.companion : undefined,
          activeTool: data.phase === 'start' ? data.tool : undefined,
        }));
      });

      source.addEventListener('approval_requested', () => {
        setRuntime((prev) => ({ ...prev, activeCompanion: 'pebble' }));
        void refresh();
      });

      source.addEventListener('approval_resolved', () => {
        setRuntime((prev) => ({ ...prev, activeCompanion: undefined }));
        void refresh();
      });

      source.addEventListener('task_updated', () => {
        setRuntime((prev) => ({ ...prev, activeCompanion: undefined, activeTool: undefined }));
        void refresh();
      });

      source.addEventListener('memory_changed', () => {
        void refresh();
      });

      source.addEventListener('reminder_due', (event) => {
        const data = JSON.parse((event as MessageEvent<string>).data) as {
          id: string;
          title: string;
        };
        setState((prev) => ({
          ...prev,
          notifications: [
            {
              id: `rem:${data.id}`,
              eventKey: `rem:${data.id}`,
              conversationId: null,
              taskId: null,
              kind: 'completed',
              read: false,
              title: data.title,
              detail: prev.settings.language === 'zh' ? '到点提醒 · Pip' : 'Reminder due · Pip',
            },
            ...prev.notifications.filter((n) => n.eventKey !== `rem:${data.id}`),
          ],
        }));
        void refresh();
      });
    });

    return () => {
      disposed = true;
      source?.close();
    };
  }, []);

  // Offline browser demo ticker (inactive when connected to live C++26 runtime).
  useEffect(() => {
    const tick = () => {
      if (liveRef.current) return;
      setState((previous) => {
        let next = previous;
        for (const c of previous.connections)
          if (c.status === 'connecting') next = settleConnection(next, c.id, c.attempt);
        for (const t of previous.tasks)
          if (active(t.status)) next = advanceDemo(next, t.id, t.attempt, Date.now());
        return next;
      });
    };
    let timer = window.setInterval(tick, 700);
    const stop = () => window.clearInterval(timer);
    const restore = (event: PageTransitionEvent) => {
      if (!event.persisted || liveRef.current) return;
      stop();
      setState((previous) => recover(previous));
      timer = window.setInterval(tick, 700);
    };
    window.addEventListener('pagehide', stop);
    window.addEventListener('pageshow', restore);
    return () => {
      stop();
      window.removeEventListener('pagehide', stop);
      window.removeEventListener('pageshow', restore);
    };
  }, []);

  const dispatch = (action: Command) => {
    if (!liveRef.current) {
      setState((previous) => command(previous, action));
      return;
    }

    // Optimistic local UI update + live C++26 Personal Agent host sync.
    const convLocator = (conversationId: string) =>
      state.conversations.find((c) => c.id === conversationId)?.locator;

    if (action.type === 'new') {
      void createServerConversation().then(async (created) => {
        if (!created) return;
        const snap = await fetchServerState();
        if (snap) {
          setState((prev) => ({
            ...mergeServerSnapshot(prev, snap),
            selectedId: conversationKey('desktop', created.id),
          }));
        }
      });
      return;
    }
    if (action.type === 'rename') {
      const loc = convLocator(action.conversationId);
      setState((previous) => command(previous, action));
      if (loc) void renameServerConversation(loc, action.name.trim());
      return;
    }
    if (action.type === 'send') {
      const loc = convLocator(action.conversationId);
      setState((previous) => ({
        ...previous,
        drafts: { ...previous.drafts, [action.conversationId]: '' },
        messages: [
          ...previous.messages,
          {
            id: `${action.id}:user`,
            conversationId: action.conversationId,
            sessionId: loc ?? action.conversationId,
            role: 'user',
            text: action.text.trim(),
            createdAt: action.now,
            state: 'complete',
          },
        ],
      }));
      if (loc) {
        void sendServerMessage(loc, action.text.trim()).then(async () => {
          const snap = await fetchServerState();
          if (snap) setState((prev) => mergeServerSnapshot(prev, snap));
        });
      }
      return;
    }
    if (action.type === 'stop') {
      void stopServerTask(action.taskId);
      return;
    }
    if (action.type === 'approve' || action.type === 'reject') {
      void resolveServerApproval(action.approvalId, action.type);
      return;
    }
    if (action.type === 'note-save' || action.type === 'note-add') {
      void upsertServerMemory({
        id: action.type === 'note-save' ? action.note.id : undefined,
        scope: action.note.scope,
        title: action.note.title,
        body: action.note.body,
      }).then(async () => {
        const snap = await fetchServerState();
        if (snap) setState((prev) => mergeServerSnapshot(prev, snap));
      });
      return;
    }
    if (action.type === 'note-delete') {
      const note = state.notes.find((n) => n.id === action.id);
      void deleteServerMemory(action.id, note?.scope ?? 'personal').then(async () => {
        const snap = await fetchServerState();
        if (snap) setState((prev) => mergeServerSnapshot(prev, snap));
      });
      return;
    }
    if (action.type === 'reminder-add') {
      void addServerReminder(action.title, action.dueAt).then(async () => {
        const snap = await fetchServerState();
        if (snap) setState((prev) => mergeServerSnapshot(prev, snap));
      });
      return;
    }
    if (action.type === 'reminder-toggle') {
      const item = (state.reminders ?? []).find((r) => r.id === action.id);
      if (item) {
        void toggleServerReminder(action.id, !item.completed).then(async () => {
          const snap = await fetchServerState();
          if (snap) setState((prev) => mergeServerSnapshot(prev, snap));
        });
      }
      return;
    }
    if (action.type === 'reminder-delete') {
      void deleteServerReminder(action.id).then(async () => {
        const snap = await fetchServerState();
        if (snap) setState((prev) => mergeServerSnapshot(prev, snap));
      });
      return;
    }
    if (action.type === 'settings' && action.value.permission) {
      void updateServerSettings(action.value.permission);
    }
    setState((previous) => command(previous, action));
  };

  const reset = () => {
    const fresh = resetState(browserStorage);
    if (!fresh) {
      setStorageError('write');
      return;
    }
    blocked.current = false;
    setStorageError(null);
    setState(fresh);
  };

  const exportSnapshot = () => {
    let raw: string | null;
    try {
      raw = browserStorage.getItem(STORAGE_KEY);
    } catch {
      setStorageError('unavailable');
      return;
    }
    const url = URL.createObjectURL(
      new Blob([raw ?? JSON.stringify(state, null, 2)], { type: 'application/json' }),
    );
    const link = document.createElement('a');
    link.href = url;
    link.download = 'orangutan-backup.json';
    link.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  };

  return { state, runtime, dispatch, storageError, reset, exportSnapshot };
}
