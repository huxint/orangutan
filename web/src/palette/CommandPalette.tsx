import { useEffect, useMemo, useRef, useState } from "react";
import { createPortal } from "react-dom";
import { useWorkspace, useWorkspaceState } from "../state/WorkspaceProvider";
import { fuzzyScore } from "../lib/utils";
import { toggleTheme } from "../theme";

interface Command {
  id: string;
  label: string;
  hint?: string;
  group: "agent" | "session" | "view" | "system";
  action: () => void;
}

export function CommandPalette() {
  const store = useWorkspace();
  const open = useWorkspaceState((s) => s.paletteOpen);
  const agents = useWorkspaceState((s) => s.agents);
  const sessions = useWorkspaceState((s) => s.sessions);
  const [query, setQuery] = useState("");
  const [active, setActive] = useState(0);
  const inputRef = useRef<HTMLInputElement | null>(null);

  useEffect(() => {
    if (open) {
      setQuery("");
      setActive(0);
      setTimeout(() => inputRef.current?.focus(), 0);
    }
  }, [open]);

  const commands = useMemo<Command[]>(() => {
    const cmds: Command[] = [];
    agents.forEach((a) => {
      cmds.push({
        id: `open:${a.key}`,
        label: `open ${a.key}`,
        hint: a.model,
        group: "agent",
        action: () => {
          store.setFocusAgent(a.key);
          store.openSession(a.key);
          store.setMode("workspace");
          store.setPaletteOpen(false);
        },
      });
      cmds.push({
        id: `new:${a.key}`,
        label: `new session · ${a.key}`,
        hint: "fresh chat",
        group: "session",
        action: () => {
          store.openSession(a.key, `draft-${a.key}-${Date.now()}`);
          store.setPaletteOpen(false);
        },
      });
    });
    for (const session of sessions.values()) {
      cmds.push({
        id: `focus:${session.id}`,
        label: `focus · ${session.id.slice(0, 18)}`,
        hint: session.agentKey,
        group: "session",
        action: () => {
          store.setFocusSession(session.id);
          store.setPaletteOpen(false);
        },
      });
    }
    cmds.push(
      {
        id: "view:workspace",
        label: "workspace",
        hint: "constellation of agents",
        group: "view",
        action: () => {
          store.setMode("workspace");
          store.setPaletteOpen(false);
        },
      },
      {
        id: "view:observatory",
        label: "observatory",
        hint: "live event feed, everything at once",
        group: "view",
        action: () => {
          store.setMode("observatory");
          store.setPaletteOpen(false);
        },
      },
      {
        id: "system:toggle-theme",
        label: "toggle theme",
        hint: "dark ↔ paper",
        group: "system",
        action: () => {
          toggleTheme();
          store.setPaletteOpen(false);
        },
      },
    );
    return cmds;
  }, [agents, sessions, store]);

  const filtered = useMemo(() => {
    if (!query.trim()) return commands;
    return commands
      .map((c) => ({
        cmd: c,
        score: Math.max(fuzzyScore(c.label, query), fuzzyScore(c.hint ?? "", query) * 0.6),
      }))
      .filter((c) => c.score > 0)
      .sort((a, b) => b.score - a.score)
      .map((c) => c.cmd);
  }, [query, commands]);

  useEffect(() => {
    if (active >= filtered.length) setActive(Math.max(0, filtered.length - 1));
  }, [filtered.length, active]);

  if (!open) return null;
  const portalTarget = document.getElementById("portal") ?? document.body;

  return createPortal(
    <div
      className="fixed inset-0 z-50 flex items-start justify-center pt-[14vh] px-6"
      onClick={() => store.setPaletteOpen(false)}
    >
      <div
        className="absolute inset-0 backdrop-blur-md bg-black/40"
        style={{ animation: "fade-up 240ms cubic-bezier(0.16, 1, 0.3, 1)" }}
      />
      <div
        onClick={(e) => e.stopPropagation()}
        className="relative surface-elevated rounded-2xl w-[640px] max-w-[92vw] overflow-hidden anim-fade-up"
      >
        <div className="flex items-center gap-3 px-5 py-4 border-b border-[var(--color-line)]">
          <span className="text-[var(--color-text-faint)] font-mono text-[13px]">⌘K</span>
          <input
            ref={inputRef}
            value={query}
            onChange={(e) => {
              setQuery(e.target.value);
              setActive(0);
            }}
            onKeyDown={(e) => {
              if (e.key === "Escape") {
                store.setPaletteOpen(false);
              } else if (e.key === "ArrowDown") {
                e.preventDefault();
                setActive((a) => Math.min(filtered.length - 1, a + 1));
              } else if (e.key === "ArrowUp") {
                e.preventDefault();
                setActive((a) => Math.max(0, a - 1));
              } else if (e.key === "Enter") {
                e.preventDefault();
                filtered[active]?.action();
              }
            }}
            placeholder="jump, spawn, observe…"
            className="flex-1 bg-transparent outline-none font-serif text-[20px] placeholder:text-[var(--color-text-faint)]"
            style={{ fontVariationSettings: '"opsz" 48' }}
          />
        </div>
        <div className="max-h-[50vh] overflow-y-auto py-2">
          {filtered.length === 0 && (
            <div className="px-5 py-6 text-[var(--color-text-dim)] font-serif">
              nothing matches — try fewer letters.
            </div>
          )}
          {filtered.map((c, i) => (
            <button
              key={c.id}
              onMouseEnter={() => setActive(i)}
              onClick={() => c.action()}
              className={`w-full text-left px-5 py-2 flex items-baseline gap-3 transition-colors ${
                i === active
                  ? "bg-[var(--color-accent-soft)] text-[var(--color-text)]"
                  : "text-[var(--color-text-dim)] hover:text-[var(--color-text)]"
              }`}
            >
              <span className="font-serif text-[16px] flex-1 truncate">{c.label}</span>
              {c.hint && (
                <span className="font-mono text-[11px] text-[var(--color-text-faint)] truncate">
                  {c.hint}
                </span>
              )}
              <span className="font-mono text-[10px] uppercase tracking-[0.22em] text-[var(--color-text-faint)]">
                {c.group}
              </span>
            </button>
          ))}
        </div>
      </div>
    </div>,
    portalTarget,
  );
}
