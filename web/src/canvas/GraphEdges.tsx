import { memo } from "react";
import type { AgentGraph } from "../api/types";
import type { GraphLayout } from "../lib/graphLayout";

interface Props {
  graph: AgentGraph;
  positions: GraphLayout;
  toScreen: (x: number, y: number) => { x: number; y: number };
  zoom: number;
}

function EdgesImpl({ graph, positions, toScreen, zoom }: Props) {
  return (
    <g>
      {graph.edges.map((edge, i) => {
        const a = positions.nodes.get(edge.source);
        const b = positions.nodes.get(edge.target);
        if (!a || !b) return null;
        const sa = toScreen(a.x, a.y);
        const sb = toScreen(b.x, b.y);
        const dx = sb.x - sa.x;
        const dy = sb.y - sa.y;
        const mx = (sa.x + sb.x) / 2;
        const my = (sa.y + sb.y) / 2;
        const bow = Math.hypot(dx, dy) * 0.12;
        const nx = -dy / Math.hypot(dx, dy);
        const ny = dx / Math.hypot(dx, dy);
        const cx = mx + nx * bow;
        const cy = my + ny * bow;
        const stroke = `color-mix(in srgb, var(--color-aux) ${Math.round(30 + zoom * 20)}%, transparent)`;
        return (
          <path
            key={`${edge.source}->${edge.target}-${i}`}
            d={`M ${sa.x} ${sa.y} Q ${cx} ${cy} ${sb.x} ${sb.y}`}
            fill="none"
            stroke={stroke}
            strokeWidth={0.75 + zoom * 0.45}
            strokeLinecap="round"
            strokeDasharray={edge.kind === "team" ? "none" : "4 5"}
            opacity={0.9}
          />
        );
      })}
    </g>
  );
}

export const GraphEdges = memo(EdgesImpl);
