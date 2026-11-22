import { stateSchema, type AppState } from './schema.ts';
import { createInitialState } from './fixtures.ts';
import { recover } from './model.ts';

export const STORAGE_KEY = 'orangutan.desktop.demo';
export interface StoragePort {
  getItem(key: string): string | null;
  setItem(key: string, value: string): void;
}
export function loadState(storage: StoragePort): {
  state: AppState;
  error: string | null;
  blocked: boolean;
} {
  try {
    const raw = storage.getItem(STORAGE_KEY);
    if (raw === null) return { state: createInitialState(), error: null, blocked: false };
    const parsed = stateSchema.safeParse(JSON.parse(raw));
    if (!parsed.success) return { state: createInitialState(), error: 'invalid', blocked: true };
    return { state: recover(parsed.data), error: null, blocked: false };
  } catch {
    return { state: createInitialState(), error: 'unavailable', blocked: true };
  }
}
export function saveState(storage: StoragePort, state: AppState): boolean {
  try {
    storage.setItem(STORAGE_KEY, JSON.stringify(state));
    return true;
  } catch {
    return false;
  }
}
export function resetState(storage: StoragePort, now = Date.now()): AppState | null {
  try {
    const old = storage.getItem(STORAGE_KEY);
    if (old !== null) storage.setItem(`${STORAGE_KEY}.backup.${now}`, old);
    const state = createInitialState(now);
    return saveState(storage, state) ? state : null;
  } catch {
    return null;
  }
}
