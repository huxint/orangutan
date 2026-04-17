import { useEffect, useState } from "react";
import { toggleTheme, getTheme, type Theme } from "../theme";
import { useWorkspace, useWorkspaceState } from "../state/WorkspaceProvider";
import { useEventBusStatus } from "../lib/useEventBus";
import { cn } from "../lib/utils";

export function HUD() {
  const [theme, setTheme] = useState<Theme>(getTheme());
  const store = useWorkspace();
  const mode = useWorkspaceState((s) => s.mode);
  const connected = useEventBusStatus();

  useEffect(() => {
    const handler = (e: Event) => {
      setTheme((e as CustomEvent<Theme>).detail);
    };
    window.addEventListener("orangutan:theme", handler);
    return () => window.removeEventListener("orangutan:theme", handler);
  }, []);

  return (
    <header className="pointer-events-none absolute top-0 left-0 right-0 z-30 flex items-center justify-between px-6 pt-5">
      <div className="pointer-events-auto flex items-center gap-3">
        <div className="flex items-baseline gap-2">
          <span className="title-display text-[20px] tracking-[-0.03em]">
            orangutan
          </span>
          <span className="text-[10px] uppercase tracking-[0.3em] text-[var(--color-text-faint)] font-mono">
            workspace
          </span>
        </div>
        <span
          title={connected ? "event bus connected" : "event bus offline"}
          className={cn(
            "inline-block w-1.5 h-1.5 rounded-full",
            connected ? "bg-[var(--color-life)] anim-breathe" : "bg-[var(--color-warn)]",
          )}
        />
      </div>

      <div className="pointer-events-auto flex items-center gap-2">
        <button
          onClick={() => store.setMode(mode === "observatory" ? "workspace" : "observatory")}
          className="chip"
          data-state={mode === "observatory" ? "active" : ""}
        >
          observatory
        </button>
        <button
          onClick={() => store.setPaletteOpen(true)}
          className="chip"
        >
          ⌘K
        </button>
        <button
          onClick={() => {
            const next = toggleTheme();
            setTheme(next);
          }}
          className="chip"
          title="toggle theme"
        >
          {theme === "dark" ? "paper" : "ink"}
        </button>
      </div>
    </header>
  );
}
