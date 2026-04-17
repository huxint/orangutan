import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { DEFAULT_CAMERA, type Camera, zoomAround } from "../lib/camera";
import { clamp } from "../lib/utils";
import { layoutGraph } from "../lib/graphLayout";
import { useWorkspaceState } from "../state/WorkspaceProvider";
import { AgentNode } from "./AgentNode";
import { SessionSatellite } from "./SessionSatellite";
import { GraphEdges } from "./GraphEdges";

interface Viewport {
  w: number;
  h: number;
}

export function Canvas() {
  const rootRef = useRef<HTMLDivElement | null>(null);
  const [camera, setCamera] = useState<Camera>(DEFAULT_CAMERA);
  const [viewport, setViewport] = useState<Viewport>({ w: 0, h: 0 });
  const dragRef = useRef<{ sx: number; sy: number; cx: number; cy: number } | null>(null);

  const graph = useWorkspaceState((s) => s.graph);
  const mode = useWorkspaceState((s) => s.mode);
  const positions = useMemo(() => (graph ? layoutGraph(graph) : null), [graph]);

  useEffect(() => {
    const el = rootRef.current;
    if (!el) return;
    const ro = new ResizeObserver(() => {
      const rect = el.getBoundingClientRect();
      setViewport({ w: rect.width, h: rect.height });
    });
    ro.observe(el);
    const rect = el.getBoundingClientRect();
    setViewport({ w: rect.width, h: rect.height });
    return () => ro.disconnect();
  }, []);

  useEffect(() => {
    setCamera((cam) => (mode === "observatory" ? { ...cam, zoom: 0.45 } : { ...cam, zoom: 1 }));
  }, [mode]);

  const onWheel = useCallback(
    (e: React.WheelEvent) => {
      if (e.ctrlKey || e.metaKey) {
        const rect = rootRef.current?.getBoundingClientRect();
        if (!rect) return;
        const anchor = { x: e.clientX - rect.left, y: e.clientY - rect.top };
        const factor = Math.exp(-e.deltaY * 0.002);
        setCamera((cam) => zoomAround(cam, anchor, viewport, factor));
        e.preventDefault();
        return;
      }
      setCamera((cam) => ({
        ...cam,
        cx: cam.cx + e.deltaX / cam.zoom,
        cy: cam.cy + e.deltaY / cam.zoom,
      }));
    },
    [viewport],
  );

  const onPointerDown = useCallback((e: React.PointerEvent) => {
    if (e.button !== 0 && e.button !== 1) return;
    const target = e.target as HTMLElement;
    if (!("canvasBackground" in target.dataset)) return;
    const host = e.currentTarget as HTMLElement;
    dragRef.current = {
      sx: e.clientX,
      sy: e.clientY,
      cx: Number(host.dataset.cx ?? 0),
      cy: Number(host.dataset.cy ?? 0),
    };
    host.setPointerCapture(e.pointerId);
  }, []);

  const onPointerMove = useCallback((e: React.PointerEvent) => {
    const drag = dragRef.current;
    if (!drag) return;
    setCamera((cam) => ({
      ...cam,
      cx: drag.cx + (drag.sx - e.clientX) / cam.zoom,
      cy: drag.cy + (drag.sy - e.clientY) / cam.zoom,
    }));
  }, []);

  const onPointerUp = useCallback((e: React.PointerEvent) => {
    if (dragRef.current) {
      dragRef.current = null;
      (e.currentTarget as HTMLElement).releasePointerCapture(e.pointerId);
    }
  }, []);

  const focusOn = useCallback(
    (x: number, y: number) => {
      setCamera((cam) => ({ ...cam, cx: x, cy: y, zoom: clamp(cam.zoom, 0.8, 1.6) }));
    },
    [],
  );

  useEffect(() => {
    const handler = (e: Event) => {
      const detail = (e as CustomEvent<{ x: number; y: number }>).detail;
      focusOn(detail.x, detail.y);
    };
    window.addEventListener("orangutan:focus-canvas", handler);
    return () => window.removeEventListener("orangutan:focus-canvas", handler);
  }, [focusOn]);

  const world = (x: number, y: number) => ({
    x: (x - camera.cx) * camera.zoom + viewport.w / 2,
    y: (y - camera.cy) * camera.zoom + viewport.h / 2,
  });

  return (
    <div
      ref={rootRef}
      className="canvas-root grain absolute inset-0 overflow-hidden"
      data-cx={camera.cx}
      data-cy={camera.cy}
      onWheel={onWheel}
      onPointerDown={onPointerDown}
      onPointerMove={onPointerMove}
      onPointerUp={onPointerUp}
      onPointerCancel={onPointerUp}
      style={
        {
          "--zoom": camera.zoom,
          "--pan-x": -camera.cx * camera.zoom + viewport.w / 2,
          "--pan-y": -camera.cy * camera.zoom + viewport.h / 2,
          cursor: dragRef.current ? "grabbing" : "default",
        } as React.CSSProperties
      }
    >
      <div
        data-canvas-background
        className="canvas-grid absolute inset-0"
        style={{ cursor: "grab" }}
      />

      {graph && positions && (
        <svg
          className="absolute inset-0 pointer-events-none"
          width={viewport.w}
          height={viewport.h}
          style={{ overflow: "visible" }}
        >
          <GraphEdges
            graph={graph}
            positions={positions}
            toScreen={world}
            zoom={camera.zoom}
          />
        </svg>
      )}

      {graph?.nodes.map((node) => {
        const pos = positions?.nodes.get(node.id);
        if (!pos) return null;
        const screen = world(pos.x, pos.y);
        return (
          <AgentNode
            key={node.id}
            node={node}
            screenX={screen.x}
            screenY={screen.y}
            zoom={camera.zoom}
            worldX={pos.x}
            worldY={pos.y}
          />
        );
      })}

      <SessionSatellite positions={positions} toScreen={world} zoom={camera.zoom} />
    </div>
  );
}
