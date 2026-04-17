import { memo } from "react";
import type { AgentGraphNode } from "../api/types";
import { cn, deterministicHue } from "../lib/utils";
import { useWorkspace, useWorkspaceState } from "../state/WorkspaceProvider";

interface Props {
  node: AgentGraphNode;
  screenX: number;
  screenY: number;
  zoom: number;
  worldX: number;
  worldY: number;
}

function NodeImpl({ node, screenX, screenY, zoom, worldX, worldY }: Props) {
  const store = useWorkspace();
  const focusAgent = useWorkspaceState((s) => s.focusAgent);
  const isFocused = focusAgent === node.id;
  const hue = deterministicHue(node.id);
  const live = node.live_sessions > 0;

  const radius = 44 * zoom;
  const label = node.id;

  const handleClick = () => {
    store.setFocusAgent(node.id);
    store.openSession(node.id);
    window.dispatchEvent(
      new CustomEvent("orangutan:focus-canvas", {
        detail: { x: worldX, y: worldY + 160 },
      }),
    );
  };

  return (
    <div
      className="absolute pointer-events-auto"
      style={{
        left: screenX,
        top: screenY,
        transform: "translate(-50%, -50%)",
      }}
    >
      <button
        onClick={handleClick}
        className={cn(
          "group relative grid place-items-center rounded-full transition-transform duration-300",
          isFocused && "scale-110",
        )}
        style={{ width: radius * 2, height: radius * 2 }}
      >
        {live && (
          <span
            className="absolute inset-0 rounded-full"
            style={{
              background: `radial-gradient(circle, hsla(${hue}deg 90% 65% / 0.25), transparent 70%)`,
              animation: "breathe 3.2s ease-in-out infinite",
            }}
          />
        )}
        <span
          className="absolute inset-[12%] rounded-full"
          style={{
            background: `conic-gradient(from ${hue}deg, hsla(${hue}deg 85% 60% / 0.8), hsla(${(hue + 120) % 360}deg 85% 60% / 0.8), hsla(${(hue + 240) % 360}deg 85% 60% / 0.8), hsla(${hue}deg 85% 60% / 0.8))`,
            opacity: isFocused ? 1 : 0.75,
            filter: "blur(1px)",
          }}
        />
        <span
          className="absolute inset-[18%] rounded-full surface-elevated"
          style={{ backdropFilter: "blur(16px)" }}
        />
        <span
          className="relative z-10 font-serif text-[var(--color-text)]"
          style={{
            fontSize: Math.max(12, radius * 0.5),
            fontVariationSettings: '"opsz" 96',
            letterSpacing: "-0.02em",
          }}
        >
          {label.slice(0, 1).toUpperCase()}
        </span>
      </button>

      <div
        className="absolute left-1/2 -translate-x-1/2 mt-2 text-center"
        style={{ top: radius + 8 }}
      >
        <div className="font-serif title-display whitespace-nowrap"
             style={{ fontSize: 18 * Math.max(0.7, zoom) }}>
          {label}
        </div>
        <div className="text-[10px] uppercase tracking-[0.2em] text-[var(--color-text-faint)] mt-1 font-mono">
          {node.model.split("/").pop() ?? node.model}
          {node.coordinator_mode && <span className="mx-2 opacity-50">·</span>}
          {node.coordinator_mode && <span>coord</span>}
          {live && (
            <>
              <span className="mx-2 opacity-50">·</span>
              <span className="text-[var(--color-accent)]">
                {node.live_sessions} live
              </span>
            </>
          )}
        </div>
      </div>
    </div>
  );
}

export const AgentNode = memo(NodeImpl);
