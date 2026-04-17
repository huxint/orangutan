import { useEffect, useRef, useState } from "react";
import { globalBus } from "../api/events";
import type { BusEventEnvelope } from "../api/types";

/// Tapped feed of the event bus, optionally filtered by kind prefix.
export function useEventBus(
  filter?: (ev: BusEventEnvelope) => boolean,
): BusEventEnvelope[] {
  const [events, setEvents] = useState<BusEventEnvelope[]>([]);
  const filterRef = useRef(filter);
  filterRef.current = filter;

  useEffect(() => {
    globalBus.connect();
    return globalBus.subscribe((ev) => {
      if (filterRef.current && !filterRef.current(ev)) return;
      setEvents((prev) => {
        const next = [...prev, ev];
        if (next.length > 200) next.splice(0, next.length - 200);
        return next;
      });
    });
  }, []);

  return events;
}

export function useEventBusStatus(): boolean {
  const [connected, setConnected] = useState(false);
  useEffect(() => {
    globalBus.connect();
    return globalBus.onStatus(setConnected);
  }, []);
  return connected;
}
