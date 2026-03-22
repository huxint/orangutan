import { useEffect, useMemo, useState } from "react";
import {
  RotateCcw,
  Terminal,
  FileEdit,
  FolderSearch,
  Search,
  FileText,
  PenLine,
  Wrench,
  type LucideIcon,
} from "lucide-react";
import { motion } from "framer-motion";
import { apiFetch } from "../../api/client";
import { cn } from "../../lib/utils";

interface Tool {
  name: string;
  description: string;
  source: string;
}

// Map well-known tool names to icons and accent colors
const TOOL_META: Record<
  string,
  { icon: LucideIcon; color: string; bg: string }
> = {
  shell: { icon: Terminal, color: "text-emerald-400", bg: "bg-emerald-500/10" },
  read: { icon: FileText, color: "text-sky-400", bg: "bg-sky-500/10" },
  write: { icon: PenLine, color: "text-violet-400", bg: "bg-violet-500/10" },
  edit: { icon: FileEdit, color: "text-amber-400", bg: "bg-amber-500/10" },
  ls: { icon: FolderSearch, color: "text-teal-400", bg: "bg-teal-500/10" },
  grep: { icon: Search, color: "text-rose-400", bg: "bg-rose-500/10" },
};

const DEFAULT_META = {
  icon: Wrench,
  color: "text-accent",
  bg: "bg-accent-dim",
};

function getMeta(name: string) {
  return TOOL_META[name.toLowerCase()] ?? DEFAULT_META;
}

// Group tools by source
function groupBySource(tools: Tool[]): Map<string, Tool[]> {
  const map = new Map<string, Tool[]>();
  for (const t of tools) {
    const src = t.source || "unknown";
    const arr = map.get(src);
    if (arr) arr.push(t);
    else map.set(src, [t]);
  }
  return map;
}

export function ToolsPage() {
  const [tools, setTools] = useState<Tool[]>([]);
  const [error, setError] = useState("");
  const [loading, setLoading] = useState(true);
  const [filter, setFilter] = useState("");

  const load = () => {
    setError("");
    setLoading(true);
    apiFetch<Tool[]>("/api/tools")
      .then(setTools)
      .catch((e) => setError(e.message))
      .finally(() => setLoading(false));
  };

  useEffect(load, []);

  const filtered = useMemo(() => {
    if (!filter) return tools;
    const q = filter.toLowerCase();
    return tools.filter(
      (t) =>
        t.name.toLowerCase().includes(q) ||
        t.description.toLowerCase().includes(q) ||
        t.source.toLowerCase().includes(q),
    );
  }, [tools, filter]);

  const groups = useMemo(() => groupBySource(filtered), [filtered]);

  return (
    <div className="p-6 h-full overflow-y-auto pb-10">
      {/* Header */}
      <div className="flex items-center justify-between mb-5">
        <div>
          <h1 className="text-xl font-bold">Tools</h1>
          {!loading && !error && (
            <p className="text-xs text-text-muted mt-0.5">
              {tools.length} tools registered across{" "}
              {new Set(tools.map((t) => t.source)).size} sources
            </p>
          )}
        </div>
      </div>

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
      ) : loading ? (
        <div className="flex items-center gap-2 text-sm text-text-muted">
          <div className="w-3.5 h-3.5 rounded-full border-2 border-accent border-t-transparent animate-spin" />{" "}
          Loading...
        </div>
      ) : (
        <>
          {/* Search */}
          {tools.length > 6 && (
            <div className="mb-4">
              <input
                type="text"
                value={filter}
                onChange={(e) => setFilter(e.target.value)}
                placeholder="Filter tools..."
                className="w-full max-w-sm rounded-lg border border-border bg-bg-surface px-3 py-2 text-sm text-text placeholder:text-text-muted focus:outline-none focus:border-accent/40 transition-colors"
              />
            </div>
          )}

          {/* Grouped cards */}
          <div className="space-y-6">
            {Array.from(groups.entries()).map(([source, sourceTools]) => (
              <motion.div
                key={source}
                initial={{ opacity: 0, y: 8 }}
                animate={{ opacity: 1, y: 0 }}
                transition={{ duration: 0.25 }}
              >
                <div className="flex items-center gap-2 mb-3">
                  <span className="text-[10px] font-bold uppercase tracking-[0.15em] text-text-muted">
                    {source}
                  </span>
                  <div className="flex-1 h-px bg-border" />
                  <span className="text-[10px] text-text-muted">
                    {sourceTools.length}
                  </span>
                </div>

                <div className="grid gap-2 sm:grid-cols-2 lg:grid-cols-3">
                  {sourceTools.map((t, i) => {
                    const meta = getMeta(t.name);
                    const Icon = meta.icon;

                    return (
                      <motion.div
                        key={t.name}
                        initial={{ opacity: 0, y: 6 }}
                        animate={{ opacity: 1, y: 0 }}
                        transition={{ delay: i * 0.03, duration: 0.2 }}
                        className={cn(
                          "group rounded-xl border border-border bg-bg-surface p-3.5",
                          "hover:border-accent/20 hover:shadow-[0_0_24px_rgba(249,115,22,0.04)]",
                          "transition-all duration-200",
                        )}
                      >
                        <div className="flex items-start gap-3">
                          <div
                            className={cn("shrink-0 p-2 rounded-lg", meta.bg)}
                          >
                            <Icon size={16} className={meta.color} />
                          </div>
                          <div className="min-w-0 flex-1">
                            <code
                              className={cn(
                                "font-mono text-sm font-bold",
                                meta.color,
                              )}
                            >
                              {t.name}
                            </code>
                            <div className="grid grid-rows-[0fr] group-hover:grid-rows-[1fr] transition-[grid-template-rows] duration-200">
                              <div className="overflow-hidden">
                                <p className="mt-1.5 text-xs text-text-secondary leading-relaxed">
                                  {t.description}
                                </p>
                              </div>
                            </div>
                          </div>
                        </div>
                      </motion.div>
                    );
                  })}
                </div>
              </motion.div>
            ))}
          </div>

          {filtered.length === 0 && filter && (
            <div className="text-center text-sm text-text-muted py-12">
              No tools matching "<span className="text-text">{filter}</span>"
            </div>
          )}
        </>
      )}
    </div>
  );
}
