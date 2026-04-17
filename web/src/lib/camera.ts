import { clamp } from "./utils";

export interface Camera {
  cx: number;
  cy: number;
  zoom: number;
}

export const DEFAULT_CAMERA: Camera = { cx: 0, cy: 0, zoom: 1 };

export function worldToScreen(
  camera: Camera,
  viewport: { w: number; h: number },
  world: { x: number; y: number },
): { x: number; y: number } {
  return {
    x: (world.x - camera.cx) * camera.zoom + viewport.w / 2,
    y: (world.y - camera.cy) * camera.zoom + viewport.h / 2,
  };
}

export function screenToWorld(
  camera: Camera,
  viewport: { w: number; h: number },
  screen: { x: number; y: number },
): { x: number; y: number } {
  return {
    x: (screen.x - viewport.w / 2) / camera.zoom + camera.cx,
    y: (screen.y - viewport.h / 2) / camera.zoom + camera.cy,
  };
}

export function zoomAround(
  camera: Camera,
  anchor: { x: number; y: number },
  viewport: { w: number; h: number },
  factor: number,
): Camera {
  const world = screenToWorld(camera, viewport, anchor);
  const nextZoom = clamp(camera.zoom * factor, 0.18, 3.2);
  const scale = nextZoom / camera.zoom;
  const cx = world.x - (world.x - camera.cx) / scale;
  const cy = world.y - (world.y - camera.cy) / scale;
  return { cx, cy, zoom: nextZoom };
}
