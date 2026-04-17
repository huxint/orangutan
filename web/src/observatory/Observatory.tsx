import { useEffect, useMemo, useState } from "react";
import { useEventBus, useEventBusStatus } from "../lib/useEventBus";
import { useWorkspace, useWorkspaceState } from "../state/WorkspaceProvider";
import type { BusEventEnvelope } from "../api/types";
import { api } from "../api/client";
import { formatUptime } from "../lib/utils";

export function Observatory() {
  const events = useEventBus();
  const connected = useEventBusStatus();
  const store = useWorkspace();
  const agents = useWorkspaceState((s) => s.agents);
  const sessions = useWorkspaceState((s) => s.sessions);
  const [uptime, setUptime] = useState(0);
  const [activeSessions, setActiveSessions] = useState(0);

  useEffect(() => {
    let cancel = false;
    const tick = async () => {
      try {
        const status = await api.system();
        if (!cancel) {
          setUptime(status.uptime_seconds);
          setActiveSessions(status.active_web_sessions);
        }
      } catch { /* ignore */ }
    };
    void tick();
    const handle = window.setInterval(tick, 6000);
    return () => {
      cancel = true;
      window.clearInterval(handle);
    };
  }, []);

  const byAgent = useMemo(() => {
    const map = new Map<string, number>();
    for (let i = events.length - 1; i >= Math.max(0, events.length - 80); i--) {
      const key = String(events[i].payload?.agent_key ?? "");
      if (!key) continue;
      map.set(key, (map.get(key) ?? 0) + 1);
    }
    return map;
  }, [events]);

  return (
    <div className="absolute inset-0 canvas-root grain anim-fade-up">
      <div className="relative h-full flex">
        <aside className="w-[380px] flex flex-col border-r border-[var(--color-line)] px-8 py-10">
          <div className="text-[10px] uppercase tracking-[0.3em] text-[var(--color-text-faint)] font-mono">
            orangutan · observatory
          </div>
          <h1 className="title-display text-[56px] mt-3">
            live
            <br />
            <span className="text-[var(--color-accent)]">at a glance</span>
          </h1>

          <div className="mt-10 space-y-5 font-mono text-[12px]">
            <Stat label="event bus" value={connected ? "connected" : "offline"} accent={connected} />
            <Stat label="uptime" value={formatUptime(uptime)} />
            <Stat label="server sessions" value={String(activeSessions)} />
            <Stat label="local cards" value={String(sessions.size)} />
            <Stat label="agents" value={String(agents.length)} />
          </div>

          <button
            onClick={() => store.setMode("workspace")}
            className="chip mt-8 self-start"
          >
            ← back to workspace
          </button>
        </aside>

        <main className="flex-1 grid grid-cols-[1.1fr_0.9fr] gap-10 p-10 overflow-hidden">
          <section className="relative surface rounded-3xl p-6 overflow-hidden">
            <div className="text-[10px] uppercase tracking-[0.3em] text-[var(--color-text-faint)] font-mono">
              agent pulse
            </div>
            <div className="mt-6 grid grid-cols-2 gap-5 content-start">
              {agents.map((a) => (
                <AgentPulse
                  key={a.key}
                  name={a.key}
                  count={byAgent.get(a.key) ?? 0}
                  model={a.model}
                />
              ))}
            </div>
          </section>

          <section className="relative surface rounded-3xl p-6 overflow-hidden flex flex-col min-h-0">
            <div className="text-[10px] uppercase tracking-[0.3em] text-[var(--color-text-faint)] font-mono">
              event stream
            </div>
            <EventFeed events={events} />
          </section>
        </main>
      </div>
    </div>
  );
}

function Stat({
  label,
  value,
  accent,
}: {
  label: string;
  value: string;
  accent?: boolean;
}) {
  return (
    <div className="flex items-baseline justify-between gap-6">
      <span className="text-[10px] uppercase tracking-[0.24em] text-[var(--color-text-faint)]">
        {label}
      </span>
      <span
        className={`font-serif text-[22px] ${
          accent ? "text-[var(--color-life)]" : "text-[var(--color-text)]"
        }`}
        style={{ fontVariationSettings: '"opsz" 32' }}
      >
        {value}
      </span>
    </div>
  );
}

function AgentPulse({
  name,
  count,
  model,
}: {
  name: string;
  count: number;
  model: string;
}) {
  const active = count > 0;
  return (
    <div className="relative rounded-2xl border border-[var(--color-line)] bg-[var(--color-bg-1)]/70 p-4 overflow-hidden">
      {active && (
        <div
          className="absolute inset-0"
          style={{
            background:
              "radial-gradient(120% 80% at 100% 0%, var(--color-accent-soft), transparent 60%)",
          }}
        />
      )}
      <div className="relative font-serif text-[24px] leading-none"
           style={{ fontVariationSettings: '"opsz" 48' }}>
        {name}
      </div>
      <div className="relative mt-2 text-[11px] font-mono text-[var(--color-text-dim)] truncate">
        {model}
      </div>
      <div className="relative mt-4 flex items-end justify-between">
        <span className="text-[10px] uppercase tracking-[0.24em] text-[var(--color-text-faint)] font-mono">
          recent events
        </span>
        <span
          className={`font-mono text-[22px] ${
            active ? "text-[var(--color-accent)]" : "text-[var(--color-text-faint)]"
          }`}
        >
          {count}
        </span>
      </div>
    </div>
  );
}

function EventFeed({ events }: { events: BusEventEnvelope[] }) {
  const latest = [...events].slice(-60).reverse();
  if (latest.length === 0)
    return (
      <div className="mt-6 text-[var(--color-text-faint)] font-serif text-[17px]">
        waiting for signals…
      </div>
    );
  return (
    <div className="mt-4 flex-1 overflow-y-auto no-scrollbar space-y-2 text-[12px] font-mono leading-snug">
      {latest.map((ev, i) => (
        <div
          key={`${ev.kind}-${ev.timestamp}-${i}`}
          className="flex gap-3 py-1 border-b border-[var(--color-line-soft)]"
        >
          <span className="text-[var(--color-text-faint)] shrink-0 w-[7ch]">
            {formatTime(ev.timestamp)}
          </span>
          <span className="text-[var(--color-accent)] shrink-0 w-[18ch]">
            {ev.kind}
          </span>
          <span className="text-[var(--color-text-dim)] truncate">
            {summariseEvent(ev)}
          </span>
        </div>
      ))}
    </div>
  );
}

function formatTime(ms: number): string {
  const d = new Date(ms);
  return `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
}

function pad(n: number): string {
  return String(n).padStart(2, "0");
}

function summariseEvent(ev: BusEventEnvelope): string {
  const p = ev.payload ?? {};
  const bits: string[] = [];
  if (typeof p.agent_key === "string") bits.push(`agent=${p.agent_key}`);
  if (typeof p.session_id === "string") bits.push(`session=${p.session_id.slice(0, 10)}`);
  if (typeof p.tool === "string") bits.push(`tool=${p.tool}`);
  if (typeof p.error === "string") bits.push(`err=${p.error.slice(0, 40)}`);
  if (typeof p.text === "string") bits.push(`+${p.text.length}`);
  return bits.join(" · ");
}
