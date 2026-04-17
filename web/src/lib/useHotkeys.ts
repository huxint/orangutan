import { useEffect } from "react";

export interface Hotkey {
  combo: string; // "mod+k", "escape", "/"
  handler: (e: KeyboardEvent) => void;
  allowInInput?: boolean;
}

const MOD = navigator.platform.toLowerCase().includes("mac") ? "metaKey" : "ctrlKey";

function matches(combo: string, e: KeyboardEvent): boolean {
  const parts = combo.toLowerCase().split("+");
  const key = parts.pop()!;
  const wantMod = parts.includes("mod");
  const wantShift = parts.includes("shift");
  const wantAlt = parts.includes("alt");

  if (wantMod !== Boolean(e[MOD])) return false;
  if (wantShift !== e.shiftKey) return false;
  if (wantAlt !== e.altKey) return false;

  if (key === "space") return e.code === "Space";
  if (key === "escape") return e.key === "Escape";
  return e.key.toLowerCase() === key;
}

export function useHotkeys(keys: Hotkey[]): void {
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      const target = e.target as HTMLElement | null;
      const inInput =
        target &&
        (target.tagName === "INPUT" ||
          target.tagName === "TEXTAREA" ||
          (target as HTMLElement).isContentEditable);
      for (const k of keys) {
        if (!matches(k.combo, e)) continue;
        if (inInput && !k.allowInInput) continue;
        e.preventDefault();
        k.handler(e);
        return;
      }
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [keys]);
}
