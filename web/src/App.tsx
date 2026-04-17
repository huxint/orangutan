import { useEffect } from "react";
import { Canvas } from "./canvas/Canvas";
import { Observatory } from "./observatory/Observatory";
import { CommandPalette } from "./palette/CommandPalette";
import { HUD } from "./hud/HUD";
import { Ticker } from "./hud/Ticker";
import { useHotkeys } from "./lib/useHotkeys";
import { toggleTheme } from "./theme";
import {
  WorkspaceProvider,
  useWorkspace,
  useWorkspaceState,
} from "./state/WorkspaceProvider";

function WorkspaceChrome() {
  const store = useWorkspace();
  const mode = useWorkspaceState((s) => s.mode);

  useHotkeys([
    {
      combo: "mod+k",
      allowInInput: true,
      handler: () => store.setPaletteOpen(!store.getState().paletteOpen),
    },
    {
      combo: "mod+e",
      allowInInput: true,
      handler: () =>
        store.setMode(
          store.getState().mode === "observatory" ? "workspace" : "observatory",
        ),
    },
    {
      combo: "mod+.",
      allowInInput: true,
      handler: () => toggleTheme(),
    },
    {
      combo: "escape",
      handler: () => {
        if (store.getState().paletteOpen) {
          store.setPaletteOpen(false);
        } else if (store.getState().mode === "observatory") {
          store.setMode("workspace");
        }
      },
    },
    {
      combo: "n",
      handler: () => {
        const focus = store.getState().focusAgent;
        if (focus) store.openSession(focus);
      },
    },
  ]);

  return (
    <div className="relative h-full w-full overflow-hidden">
      {mode === "workspace" ? <Canvas /> : <Observatory />}
      <HUD />
      <Ticker />
      <CommandPalette />
    </div>
  );
}

export default function App() {
  useEffect(() => {
    // Nothing to do today; reserved for future wiring (error boundary, etc.).
  }, []);
  return (
    <WorkspaceProvider>
      <WorkspaceChrome />
    </WorkspaceProvider>
  );
}
