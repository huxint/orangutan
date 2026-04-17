import { useEventBus } from "../lib/useEventBus";
import { useWorkspaceState } from "../state/WorkspaceProvider";

export function Ticker() {
  const events = useEventBus();
  const connected = useWorkspaceState((s) => s.connected);
  const last = events[events.length - 1];

  return (
    <footer className="pointer-events-none absolute bottom-0 left-0 right-0 z-30 px-6 pb-4 flex justify-between text-[11px] font-mono text-[var(--color-text-faint)]">
      <span>
        {connected ? "bus · live" : "bus · offline"}
      </span>
      {last && (
        <span className="max-w-[60vw] truncate">
          <span className="text-[var(--color-accent)]">{last.kind}</span>
          {" · "}
          <span>{describe(last.payload)}</span>
        </span>
      )}
      <span>
        ⌘K jump · ⌘E observatory · ⌘. theme
      </span>
    </footer>
  );
}

function describe(payload: Record<string, unknown> | undefined): string {
  if (!payload) return "";
  if (typeof payload.agent_key === "string") return payload.agent_key;
  if (typeof payload.name === "string") return payload.name;
  if (typeof payload.session_id === "string") return payload.session_id.slice(0, 16);
  return "";
}
