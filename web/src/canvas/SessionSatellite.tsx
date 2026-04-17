import { memo, useMemo } from "react";
import { useWorkspaceState } from "../state/WorkspaceProvider";
import type { GraphLayout } from "../lib/graphLayout";
import { SessionCard } from "./SessionCard";
import type { SessionState } from "../state/workspace";

interface Props {
  positions: GraphLayout | null;
  toScreen: (x: number, y: number) => { x: number; y: number };
  zoom: number;
}

function Impl({ positions, toScreen, zoom }: Props) {
  const sessions = useWorkspaceState((s) => s.sessions);
  const focusSession = useWorkspaceState((s) => s.focusSession);
  const mode = useWorkspaceState((s) => s.mode);

  const ordered = useMemo(() => {
    const arr: SessionState[] = [...sessions.values()];
    arr.sort((a, b) => (focusSession === a.id ? -1 : focusSession === b.id ? 1 : b.activityAt - a.activityAt));
    return arr;
  }, [sessions, focusSession]);

  const sessionsByAgent = useMemo(() => {
    const map = new Map<string, SessionState[]>();
    for (const s of ordered) {
      const list = map.get(s.agentKey) ?? [];
      list.push(s);
      map.set(s.agentKey, list);
    }
    return map;
  }, [ordered]);

  if (mode === "observatory") return null;

  return (
    <>
      {[...sessionsByAgent.entries()].map(([agentKey, group]) =>
        group.map((session, i) => {
          const basePos = positions?.nodes.get(agentKey);
          if (!basePos) return null;
          const focused = session.id === focusSession;
          const offsetAngle = Math.PI * 0.15 * (i + 1);
          const dx = focused ? 0 : Math.cos(offsetAngle) * 260;
          const dy = focused ? 260 : 160 + i * 30;
          const screen = toScreen(basePos.x + dx, basePos.y + dy);
          return (
            <div
              key={session.id}
              className="absolute anim-fade-up"
              style={{
                left: screen.x,
                top: screen.y,
                transform: `translate(-50%, 0) scale(${Math.max(0.7, zoom)})`,
                transformOrigin: "top center",
                zIndex: focused ? 10 : 2,
              }}
            >
              <SessionCard session={session} />
            </div>
          );
        }),
      )}
    </>
  );
}

export const SessionSatellite = memo(Impl);
