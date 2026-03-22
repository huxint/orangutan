import { useEffect, useState, useRef } from "react";
import { Activity, Clock, Users, RotateCcw } from "lucide-react";
import { motion } from "framer-motion";
import { apiFetch } from "../../api/client";

interface SystemStatus {
  uptime_seconds: number;
  active_web_sessions: number;
  provider_health: Record<string, unknown>;
  cron: Record<string, unknown>;
  heartbeat: Record<string, unknown>;
}

function formatUptime(s: number): string {
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = Math.floor(s % 60);
  return `${h}h ${m}m ${sec}s`;
}

export function SystemPage() {
  const [status, setStatus] = useState<SystemStatus | null>(null);
  const [error, setError] = useState("");
  const intervalRef = useRef<ReturnType<typeof setInterval>>(undefined);

  const load = () => {
    setError("");
    apiFetch<SystemStatus>("/api/system/status")
      .then(setStatus)
      .catch((e) => setError(e.message));
  };

  useEffect(() => {
    load();
    intervalRef.current = setInterval(load, 30000);
    return () => clearInterval(intervalRef.current);
  }, []);

  return (
    <div className="p-6 h-full overflow-y-auto pb-20">
      <h1 className="text-xl font-bold mb-4">System</h1>

      {error ? (
        <div className="rounded-xl border border-danger/20 bg-danger-dim p-4 text-danger text-sm">
          {error}{" "}
          <button
            onClick={load}
            className="ml-2 inline-flex items-center gap-1 text-xs underline"
          >
            <RotateCcw size={12} />
            Retry
          </button>
        </div>
      ) : !status ? (
        <div className="flex items-center gap-2 text-sm text-text-muted">
          <div className="w-3.5 h-3.5 rounded-full border-2 border-accent border-t-transparent animate-spin" />{" "}
          Loading...
        </div>
      ) : (
        <div className="grid grid-cols-1 sm:grid-cols-3 gap-3">
          {[
            {
              icon: Clock,
              label: "Uptime",
              value: formatUptime(status.uptime_seconds),
              delay: 0,
            },
            {
              icon: Users,
              label: "Sessions",
              value: String(status.active_web_sessions),
              delay: 0.05,
            },
          ].map((card) => (
            <motion.div
              key={card.label}
              initial={{ opacity: 0, y: 6 }}
              animate={{ opacity: 1, y: 0 }}
              transition={{ delay: card.delay }}
              className="rounded-xl border border-border bg-bg-surface p-4"
            >
              <div className="flex items-center gap-2 mb-2">
                <div className="p-1.5 rounded-lg bg-accent-dim text-accent">
                  <card.icon size={14} />
                </div>
                <span className="text-[10px] font-semibold uppercase tracking-widest text-text-muted">
                  {card.label}
                </span>
              </div>
              <div className="text-xl font-bold text-text">{card.value}</div>
            </motion.div>
          ))}

          <motion.div
            initial={{ opacity: 0, y: 6 }}
            animate={{ opacity: 1, y: 0 }}
            transition={{ delay: 0.1 }}
            className="rounded-xl border border-border bg-bg-surface p-4"
          >
            <div className="flex items-center gap-2 mb-2">
              <div className="p-1.5 rounded-lg bg-success-dim text-success">
                <Activity size={14} />
              </div>
              <span className="text-[10px] font-semibold uppercase tracking-widest text-text-muted">
                Status
              </span>
            </div>
            <div className="flex items-center gap-2 text-xl font-bold text-text">
              <span className="relative flex h-2.5 w-2.5">
                <span className="animate-ping absolute inline-flex h-full w-full rounded-full bg-success opacity-75" />
                <span className="relative inline-flex rounded-full h-2.5 w-2.5 bg-success" />
              </span>
              Healthy
            </div>
          </motion.div>
        </div>
      )}
    </div>
  );
}
