import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useRef,
  useState,
  useSyncExternalStore,
} from "react";
import type { ReactNode } from "react";
import { api, streamChat } from "../api/client";
import { globalBus } from "../api/events";
import type { AgentSummary, ChatStreamEvent } from "../api/types";
import {
  applyStreamEvent,
  initialWorkspace,
  makeEmptySession,
  type AssistantTurn,
  type SessionState,
  type UserTurn,
  type WorkspaceState,
} from "./workspace";

type Listener = () => void;

interface WorkspaceStore {
  getState(): WorkspaceState;
  subscribe(l: Listener): () => void;
  setAgents(agents: AgentSummary[]): void;
  setGraph(graph: WorkspaceState["graph"]): void;
  setMode(mode: WorkspaceState["mode"]): void;
  setFocusAgent(key: string | null): void;
  setFocusSession(id: string | null): void;
  setPaletteOpen(open: boolean): void;
  setConnected(connected: boolean): void;
  openSession(agentKey: string, sessionId?: string): SessionState;
  updateSession(id: string, fn: (s: SessionState) => SessionState): void;
  renameSession(oldId: string, newId: string): void;
  removeSession(id: string): void;
}

function createWorkspaceStore(): WorkspaceStore {
  let state: WorkspaceState = { ...initialWorkspace, sessions: new Map() };
  const listeners = new Set<Listener>();
  const emit = () => listeners.forEach((l) => l());

  function mutate(partial: Partial<WorkspaceState>) {
    state = { ...state, ...partial };
    emit();
  }

  return {
    getState: () => state,
    subscribe(l) {
      listeners.add(l);
      return () => {
        listeners.delete(l);
      };
    },
    setAgents: (agents) => mutate({ agents }),
    setGraph: (graph) => mutate({ graph }),
    setMode: (mode) => mutate({ mode }),
    setFocusAgent: (focusAgent) => mutate({ focusAgent }),
    setFocusSession: (focusSession) => mutate({ focusSession }),
    setPaletteOpen: (paletteOpen) => mutate({ paletteOpen }),
    setConnected: (connected) => mutate({ connected }),
    openSession(agentKey, sessionId) {
      const id = sessionId ?? `draft-${agentKey}-${Date.now()}`;
      if (state.sessions.has(id)) {
        mutate({ focusAgent: agentKey, focusSession: id });
        return state.sessions.get(id)!;
      }
      const next = makeEmptySession(id, agentKey);
      const map = new Map(state.sessions);
      map.set(id, next);
      state = { ...state, sessions: map, focusAgent: agentKey, focusSession: id };
      emit();
      return next;
    },
    updateSession(id, fn) {
      const existing = state.sessions.get(id);
      if (!existing) return;
      const next = fn(existing);
      if (next === existing) return;
      const map = new Map(state.sessions);
      map.set(id, next);
      state = { ...state, sessions: map };
      emit();
    },
    renameSession(oldId, newId) {
      if (oldId === newId) return;
      const existing = state.sessions.get(oldId);
      if (!existing) return;
      const map = new Map(state.sessions);
      map.delete(oldId);
      map.set(newId, { ...existing, id: newId });
      state = {
        ...state,
        sessions: map,
        focusSession: state.focusSession === oldId ? newId : state.focusSession,
      };
      emit();
    },
    removeSession(id) {
      if (!state.sessions.has(id)) return;
      const map = new Map(state.sessions);
      map.delete(id);
      state = {
        ...state,
        sessions: map,
        focusSession: state.focusSession === id ? null : state.focusSession,
      };
      emit();
    },
  };
}

const Ctx = createContext<WorkspaceStore | null>(null);

export function WorkspaceProvider({ children }: { children: ReactNode }) {
  const ref = useRef<WorkspaceStore | null>(null);
  ref.current ??= createWorkspaceStore();
  const store = ref.current;

  useEffect(() => {
    let cancelled = false;
    const load = async () => {
      try {
        const [agents, graph] = await Promise.all([api.agents(), api.graph()]);
        if (cancelled) return;
        store.setAgents(agents.items);
        store.setGraph(graph);
        if (!store.getState().focusAgent && agents.items.length > 0) {
          store.setFocusAgent(agents.items[0].key);
        }
      } catch (err) {
        console.error("workspace bootstrap failed", err);
      }
    };
    void load();
    globalBus.connect();
    const unsubStatus = globalBus.onStatus(store.setConnected);
    const refresh = () => void api.graph().then(store.setGraph).catch(() => {});
    const unsubBus = globalBus.subscribe((ev) => {
      if (
        ev.kind === "chat.session_started" ||
        ev.kind === "chat.done" ||
        ev.kind === "chat.aborted" ||
        ev.kind === "chat.error"
      ) {
        refresh();
      }
    });
    return () => {
      cancelled = true;
      unsubStatus();
      unsubBus();
    };
  }, [store]);

  return <Ctx.Provider value={store}>{children}</Ctx.Provider>;
}

export function useWorkspace(): WorkspaceStore {
  const s = useContext(Ctx);
  if (!s) throw new Error("WorkspaceProvider missing");
  return s;
}

export function useWorkspaceState<T>(selector: (s: WorkspaceState) => T): T {
  const store = useWorkspace();
  return useSyncExternalStore(
    store.subscribe,
    () => selector(store.getState()),
    () => selector(store.getState()),
  );
}

/// High-level lifecycle for submitting a message in a session.
export function useSubmitMessage(sessionId: string) {
  const store = useWorkspace();
  const abortRef = useRef<AbortController | null>(null);
  const [submitting, setSubmitting] = useState(false);

  const submit = useCallback(
    async (text: string) => {
      const trimmed = text.trim();
      if (!trimmed) return;
      const session = store.getState().sessions.get(sessionId);
      if (!session) return;

      const userTurn: UserTurn = {
        id: `u-${Date.now()}`,
        role: "user",
        text: trimmed,
      };
      const assistantTurn: AssistantTurn = {
        id: `a-${Date.now()}`,
        role: "assistant",
        segments: [],
        toolCalls: new Map(),
        streaming: true,
      };

      store.updateSession(sessionId, (s) => ({
        ...s,
        turns: [...s.turns, userTurn, assistantTurn],
        streaming: true,
        composer: "",
      }));

      const controller = new AbortController();
      abortRef.current = controller;
      setSubmitting(true);
      let activeId = sessionId;
      const draftSubmit = sessionId.startsWith("draft-");
      try {
        await streamChat({
          agentKey: session.agentKey,
          sessionId: draftSubmit ? null : sessionId,
          message: trimmed,
          signal: controller.signal,
          onEvent: (event: ChatStreamEvent) => {
            if (event.type === "session") {
              const newId = event.session_id;
              if (newId && newId !== activeId) {
                store.renameSession(activeId, newId);
                activeId = newId;
              }
              return;
            }
            store.updateSession(activeId, (prev) =>
              applyStreamEvent(prev, assistantTurn.id, event),
            );
          },
        });
      } catch (err) {
        if ((err as Error).name !== "AbortError") {
          const message = err instanceof Error ? err.message : String(err);
          store.updateSession(activeId, (prev) => ({
            ...prev,
            streaming: false,
            turns: prev.turns.map((t) =>
              t.id === assistantTurn.id && t.role === "assistant"
                ? { ...t, streaming: false, error: message }
                : t,
            ),
          }));
        }
      } finally {
        abortRef.current = null;
        setSubmitting(false);
      }
    },
    [sessionId, store],
  );

  const abort = useCallback(() => {
    abortRef.current?.abort();
    const s = store.getState().sessions.get(sessionId);
    if (s && !s.id.startsWith("draft-")) {
      void api.abortSession(s.id).catch(() => {});
    }
  }, [sessionId, store]);

  return { submit, abort, submitting };
}

export function useAgentLookup(): Map<string, AgentSummary> {
  const agents = useWorkspaceState((s) => s.agents);
  return useMemo(() => {
    const map = new Map<string, AgentSummary>();
    agents.forEach((a) => map.set(a.key, a));
    return map;
  }, [agents]);
}
