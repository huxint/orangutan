import { clsx, type ClassValue } from "clsx";
import { twMerge } from "tailwind-merge";

export function cn(...values: ClassValue[]): string {
  return twMerge(clsx(values));
}

export function deterministicHue(key: string): number {
  let h = 2166136261;
  for (let i = 0; i < key.length; i++) {
    h ^= key.charCodeAt(i);
    h = (h * 16777619) >>> 0;
  }
  return h % 360;
}

export function shortenPath(path: string, keep = 32): string {
  if (path.length <= keep) return path;
  const head = path.slice(0, 12);
  const tail = path.slice(-(keep - 14));
  return `${head}…${tail}`;
}

export function formatUptime(seconds: number): string {
  if (seconds < 60) return `${seconds}s`;
  if (seconds < 3600) return `${Math.floor(seconds / 60)}m`;
  if (seconds < 86400)
    return `${Math.floor(seconds / 3600)}h ${Math.floor((seconds % 3600) / 60)}m`;
  return `${Math.floor(seconds / 86400)}d`;
}

export function clamp(value: number, lo: number, hi: number): number {
  return Math.max(lo, Math.min(hi, value));
}

export function lerp(a: number, b: number, t: number): number {
  return a + (b - a) * t;
}

/// Fuzzy scoring — returns 0 when no match, higher = better.
export function fuzzyScore(haystack: string, needle: string): number {
  if (!needle) return 1;
  const h = haystack.toLowerCase();
  const n = needle.toLowerCase();
  if (h === n) return 1000;
  if (h.startsWith(n)) return 500 + n.length * 10;
  if (h.includes(n)) return 250 + n.length * 5;
  let hi = 0;
  let score = 0;
  let inRun = false;
  for (const ch of n) {
    let found = -1;
    for (let k = hi; k < h.length; k++) {
      if (h[k] === ch) {
        found = k;
        break;
      }
    }
    if (found === -1) return 0;
    if (inRun && found === hi) score += 4;
    else score += 1;
    inRun = found === hi;
    hi = found + 1;
  }
  return score;
}
