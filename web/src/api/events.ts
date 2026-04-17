import type { BusEventEnvelope } from "./types";

export type BusListener = (ev: BusEventEnvelope) => void;

/// Long-lived connection to /api/v1/events via EventSource.
/// Automatically reconnects and replays via `Last-Event-ID`.
export class EventBus {
  private es: EventSource | null = null;
  private listeners = new Set<BusListener>();
  private connected = false;
  private lastId = 0;
  private readonly onStatusChange = new Set<(connected: boolean) => void>();

  connect(): void {
    if (this.es !== null) return;
    this.open();
  }

  private open(): void {
    const url = new URL("/api/v1/events", window.location.origin);
    if (this.lastId > 0) url.searchParams.set("last_event_id", String(this.lastId));
    const es = new EventSource(url.toString());
    this.es = es;

    const handleAny = (evt: MessageEvent) => {
      if (evt.lastEventId) {
        const parsed = Number.parseInt(evt.lastEventId, 10);
        if (Number.isFinite(parsed)) this.lastId = parsed;
      }
      let envelope: BusEventEnvelope;
      try {
        envelope = JSON.parse(evt.data) as BusEventEnvelope;
      } catch {
        return;
      }
      if (!envelope.kind) return;
      this.listeners.forEach((l) => {
        l(envelope);
      });
    };

    es.onopen = () => {
      this.connected = true;
      this.onStatusChange.forEach((cb) => {
        cb(true);
      });
    };
    es.onerror = () => {
      if (this.connected) {
        this.connected = false;
        this.onStatusChange.forEach((cb) => {
          cb(false);
        });
      }
      // EventSource retries automatically. Recreate if the browser gave up.
      if (es.readyState === EventSource.CLOSED) {
        this.es = null;
        setTimeout(() => {
          this.open();
        }, 1200);
      }
    };

    // Server uses named events for each `kind`; fall back to message if untyped.
    const topics = [
      "ready",
      "chat.session_started",
      "chat.text",
      "chat.tool_start",
      "chat.tool_end",
      "chat.done",
      "chat.error",
      "chat.aborted",
      "automation.created",
      "automation.updated",
      "automation.deleted",
      "automation.run_started",
    ];
    topics.forEach((kind) => es.addEventListener(kind, handleAny));
    es.addEventListener("message", handleAny);
  }

  disconnect(): void {
    this.es?.close();
    this.es = null;
    this.connected = false;
  }

  subscribe(listener: BusListener): () => void {
    this.listeners.add(listener);
    return () => {
      this.listeners.delete(listener);
    };
  }

  onStatus(listener: (connected: boolean) => void): () => void {
    this.onStatusChange.add(listener);
    listener(this.connected);
    return () => {
      this.onStatusChange.delete(listener);
    };
  }
}

export const globalBus = new EventBus();
