import { createContext, useContext } from 'react';
import type { AppState } from './schema.ts';
import type { Command } from './model.ts';

export type View = 'assistant' | 'tasks' | 'connections' | 'memory' | 'settings';
export interface AppContextValue {
  state: AppState;
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
