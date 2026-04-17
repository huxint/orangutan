import type { AgentGraph } from "../api/types";

export interface AgentPosition {
  id: string;
  x: number;
  y: number;
}

export interface GraphLayout {
  nodes: Map<string, AgentPosition>;
}

/// Simple force-directed layout, run synchronously for deterministic placement.
/// Edges act as springs; all nodes repel each other; global centering pulls weakly to origin.
export function layoutGraph(graph: AgentGraph): GraphLayout {
  const width = 900;
  const ids = graph.nodes.map((n) => n.id);
  const n = ids.length;
  if (n === 0) return { nodes: new Map() };

  const positions: Record<string, { x: number; y: number; vx: number; vy: number }> = {};

  // Seed on a golden-angle spiral so we always start in a well-distributed state.
  const golden = Math.PI * (3 - Math.sqrt(5));
  ids.forEach((id, i) => {
    const r = Math.sqrt(i + 1) * 42;
    const a = i * golden;
    positions[id] = { x: Math.cos(a) * r, y: Math.sin(a) * r, vx: 0, vy: 0 };
  });

  const edges = graph.edges;
  const REPEL = 14000;
  const SPRING = 0.025;
  const SPRING_LEN = 220;
  const DAMP = 0.82;
  const CENTER = 0.0025;

  for (let iter = 0; iter < 260; iter++) {
    // Repulsion between every pair
    for (let i = 0; i < n; i++) {
      const a = positions[ids[i]];
      for (let j = i + 1; j < n; j++) {
        const b = positions[ids[j]];
        const dx = b.x - a.x;
        const dy = b.y - a.y;
        const distSq = Math.max(dx * dx + dy * dy, 25);
        const force = REPEL / distSq;
        const dist = Math.sqrt(distSq);
        const fx = (dx / dist) * force;
        const fy = (dy / dist) * force;
        a.vx -= fx;
        a.vy -= fy;
        b.vx += fx;
        b.vy += fy;
      }
    }

    // Spring along edges
    for (const edge of edges) {
      const a = positions[edge.source];
      const b = positions[edge.target];
      if (!a || !b) continue;
      const dx = b.x - a.x;
      const dy = b.y - a.y;
      const dist = Math.sqrt(dx * dx + dy * dy) || 1;
      const stretch = dist - SPRING_LEN;
      const f = stretch * SPRING;
      const fx = (dx / dist) * f;
      const fy = (dy / dist) * f;
      a.vx += fx;
      a.vy += fy;
      b.vx -= fx;
      b.vy -= fy;
    }

    // Center-pull
    for (const id of ids) {
      const p = positions[id];
      p.vx -= p.x * CENTER;
      p.vy -= p.y * CENTER;
      p.vx *= DAMP;
      p.vy *= DAMP;
      p.x += p.vx;
      p.y += p.vy;
      // clamp so it doesn't fly off
      p.x = Math.max(-width, Math.min(width, p.x));
      p.y = Math.max(-width, Math.min(width, p.y));
    }
  }

  const result = new Map<string, AgentPosition>();
  ids.forEach((id) => {
    const p = positions[id];
    result.set(id, { id, x: p.x, y: p.y });
  });
  return { nodes: result };
}
