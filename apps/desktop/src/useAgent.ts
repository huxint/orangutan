import { useEffect, useRef, useState } from 'react';
import { active, advanceDemo, command, recover, settleConnection, type Command } from './model.ts';
import { loadState, resetState, saveState, STORAGE_KEY, type StoragePort } from './storage.ts';

const browserStorage: StoragePort = {
  getItem: (key) => window.localStorage.getItem(key),
  setItem: (key, value) => window.localStorage.setItem(key, value),
};

export function useAgent() {
  const [loaded] = useState(() => loadState(browserStorage));
  const [state, setState] = useState(loaded.state);
  const [storageError, setStorageError] = useState(loaded.error);
  const blocked = useRef(loaded.blocked);
  useEffect(() => {
    if (!blocked.current) setStorageError(saveState(browserStorage, state) ? null : 'write');
  }, [state]);
  useEffect(() => {
    const tick = () => {
      setState((previous) => {
        let next = previous;
        for (const c of previous.connections)
          if (c.status === 'connecting') next = settleConnection(next, c.id, c.attempt);
        for (const t of previous.tasks)
          if (active(t.status)) next = advanceDemo(next, t.id, t.attempt, Date.now());
        return next;
      });
    };
    let timer = window.setInterval(tick, 700);
    const stop = () => window.clearInterval(timer);
    const restore = (event: PageTransitionEvent) => {
      if (!event.persisted) return;
      stop();
      setState((previous) => recover(previous));
      timer = window.setInterval(tick, 700);
    };
    window.addEventListener('pagehide', stop);
    window.addEventListener('pageshow', restore);
    return () => {
      stop();
      window.removeEventListener('pagehide', stop);
      window.removeEventListener('pageshow', restore);
    };
  }, []);
  const dispatch = (action: Command) => setState((previous) => command(previous, action));
  const reset = () => {
    const fresh = resetState(browserStorage);
    if (!fresh) {
      setStorageError('write');
      return;
    }
    blocked.current = false;
    setStorageError(null);
    setState(fresh);
  };
  const exportSnapshot = () => {
    let raw: string | null;
    try {
      raw = browserStorage.getItem(STORAGE_KEY);
    } catch {
      setStorageError('unavailable');
      return;
    }
    const url = URL.createObjectURL(
      new Blob([raw ?? JSON.stringify(state, null, 2)], { type: 'application/json' }),
    );
    const link = document.createElement('a');
    link.href = url;
    link.download = 'orangutan-demo-backup.json';
    link.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  };
  return { state, dispatch, storageError, reset, exportSnapshot };
}
