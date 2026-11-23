import { createContext, useContext } from 'react';
import type { AppState } from './schema.ts';
import type { Command } from './model.ts';
import type { PetId } from './pets.tsx';

export type View = 'assistant' | 'tasks' | 'reminders' | 'connections' | 'memory' | 'settings';
export type RuntimeMode = 'offline' | 'live-demo' | 'live';

export interface RuntimeInfo {
  mode: RuntimeMode;
  workspace?: string;
  model?: string;
  activeCompanion?: PetId;
  activeTool?: string;
}

export interface AppContextValue {
  state: AppState;
  runtime: RuntimeInfo;
  dispatch: (action: Command) => void;
  t: (zh: string, en: string) => string;
  view: View;
  navigate: (view: View) => void;
  selectConversation: (id: string) => void;
  openTask: (id: string) => void;
  toast: (message: string) => void;
}
export const AppContext = createContext<AppContextValue | null>(null);
export function useApp(): AppContextValue {
  const value = useContext(AppContext);
  if (!value) throw new Error('Application context is unavailable');
  return value;
}
