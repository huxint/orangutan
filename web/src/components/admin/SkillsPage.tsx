import { useEffect, useMemo, useState } from "react";
import {
  RotateCcw,
  Zap,
  Sparkles,
  Code2,
  Shield,
  Palette,
  Bug,
  GitBranch,
  FileSearch,
  Lightbulb,
  Blocks,
  type LucideIcon,
} from "lucide-react";
import { motion, AnimatePresence } from "framer-motion";
import { apiFetch } from "../../api/client";
import { cn } from "../../lib/utils";

interface Skill {
  id: string;
  name: string;
  description: string;
  tools: string[];
  source: string;
  scope: "always" | "conditional" | "manual_only";
  active: boolean;
  diagnostic_count: number;
  source_path: string;
}

interface SkillsResponse {
  schema_version: number;
  skills: Skill[];
}

// Map well-known skill name patterns to icons + colors
const SKILL_THEMES: {
  match: (n: string) => boolean;
  icon: LucideIcon;
  color: string;
  bg: string;
}[] = [
  {
    match: (n) => /debug|bug|fix/i.test(n),
    icon: Bug,
    color: "text-red-400",
    bg: "bg-red-500/10",
  },
  {
    match: (n) => /frontend|ui|design|css/i.test(n),
    icon: Palette,
    color: "text-pink-400",
    bg: "bg-pink-500/10",
  },
  {
    match: (n) => /api|rest|endpoint/i.test(n),
    icon: Code2,
    color: "text-sky-400",
    bg: "bg-sky-500/10",
  },
  {
    match: (n) => /security|auth|guard/i.test(n),
    icon: Shield,
    color: "text-emerald-400",
    bg: "bg-emerald-500/10",
  },
  {
    match: (n) => /git|branch|commit|pr/i.test(n),
    icon: GitBranch,
    color: "text-orange-400",
    bg: "bg-orange-500/10",
  },
  {
    match: (n) => /explore|search|find/i.test(n),
    icon: FileSearch,
    color: "text-teal-400",
    bg: "bg-teal-500/10",
  },
  {
    match: (n) => /plan|spec|architect/i.test(n),
    icon: Lightbulb,
    color: "text-amber-400",
    bg: "bg-amber-500/10",
  },
  {
    match: (n) => /test|tdd|verify/i.test(n),
    icon: Blocks,
    color: "text-cyan-400",
    bg: "bg-cyan-500/10",
  },
  {
    match: (n) => /brainstorm|idea|create/i.test(n),
    icon: Sparkles,
    color: "text-violet-400",
    bg: "bg-violet-500/10",
  },
];

const DEFAULT_THEME = { icon: Zap, color: "text-accent", bg: "bg-accent-dim" };

function getTheme(name: string) {
  return SKILL_THEMES.find((t) => t.match(name)) ?? DEFAULT_THEME;
}

export function SkillsPage() {
  const [skills, setSkills] = useState<Skill[]>([]);
  const [error, setError] = useState("");
  const [loading, setLoading] = useState(true);
  const [filter, setFilter] = useState("");
  const [expanded, setExpanded] = useState<string | null>(null);

  const load = () => {
    setError("");
    setLoading(true);
    apiFetch<SkillsResponse>("/api/skills")
      .then((response) => setSkills(response.skills))
      .catch((e) => setError(e.message))
      .finally(() => setLoading(false));
  };

  useEffect(load, []);

  const filtered = useMemo(() => {
    if (!filter) return skills;
    const q = filter.toLowerCase();
    return skills.filter(
      (s) =>
        s.name.toLowerCase().includes(q) ||
        s.description.toLowerCase().includes(q) ||
        s.tools.some((t) => t.toLowerCase().includes(q)),
    );
  }, [skills, filter]);

  return (
    <div className="p-6 h-full overflow-y-auto pb-10">
      {/* Header */}
      <div className="flex items-center justify-between mb-5">
        <div>
          <h1 className="text-xl font-bold">Skills</h1>
          {!loading && !error && (
            <p className="text-xs text-text-muted mt-0.5">
              {skills.length} skill{skills.length !== 1 ? "s" : ""} loaded
            </p>
          )}
        </div>
      </div>

      {error ? (
        <div className="rounded-xl border border-danger/20 bg-danger-dim p-4 text-danger text-sm">
          {error}
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
          <div className="w-3.5 h-3.5 rounded-full border-2 border-accent border-t-transparent animate-spin" />
          Loading...
        </div>
      ) : skills.length === 0 ? (
        <div className="flex flex-col items-center justify-center py-20 text-text-muted">
          <Zap size={32} className="mb-3 opacity-30" />
          <p className="text-sm">No skills configured</p>
          <p className="text-[11px] mt-1">
            Add skills to{" "}
            <code className="px-1 py-0.5 rounded bg-bg-elevated text-[10px] font-mono">
              ~/.orangutan/skills/
            </code>
          </p>
        </div>
      ) : (
        <>
          {/* Search */}
          {skills.length > 4 && (
            <div className="mb-4">
              <input
                type="text"
                value={filter}
                onChange={(e) => setFilter(e.target.value)}
                placeholder="Filter skills..."
                className="w-full max-w-sm rounded-lg border border-border bg-bg-surface px-3 py-2 text-sm text-text placeholder:text-text-muted focus:outline-none focus:border-accent/40 transition-colors"
              />
            </div>
          )}

          {/* Grid */}
          <div className="grid gap-2.5 sm:grid-cols-2 lg:grid-cols-3">
            {filtered.map((s, i) => {
              const theme = getTheme(s.name);
              const Icon = theme.icon;
              const isExpanded = expanded === s.name;

              return (
                <motion.div
                  key={s.name}
                  initial={{ opacity: 0, y: 6 }}
                  animate={{ opacity: 1, y: 0 }}
                  transition={{ delay: i * 0.03, duration: 0.2 }}
                  onClick={() => setExpanded(isExpanded ? null : s.name)}
                  className={cn(
                    "group rounded-xl border bg-bg-surface p-3.5 cursor-pointer",
                    "transition-all duration-200",
                    isExpanded
                      ? "border-accent/25 shadow-[0_0_24px_rgba(249,115,22,0.06)]"
                      : "border-border hover:border-accent/20 hover:shadow-[0_0_24px_rgba(249,115,22,0.04)]",
                  )}
                >
                  <div className="flex items-start gap-3">
                    <div className={cn("shrink-0 p-2 rounded-lg", theme.bg)}>
                      <Icon size={16} className={theme.color} />
                    </div>
                    <div className="min-w-0 flex-1">
                      <div className="flex items-center gap-2">
                        <code
                          className={cn(
                            "font-mono text-sm font-bold",
                            theme.color,
                          )}
                        >
                          {s.name}
                        </code>
                        {s.tools.length > 0 && (
                          <span className="text-[10px] font-semibold bg-bg-elevated text-text-muted px-1.5 py-0.5 rounded">
                            {s.tools.length} tool
                            {s.tools.length !== 1 ? "s" : ""}
                          </span>
                        )}
                      </div>

                      {/* Description: hidden by default, show on hover or expand */}
                      <div
                        className={cn(
                          "overflow-hidden transition-[grid-template-rows] duration-200",
                          isExpanded
                            ? "grid grid-rows-[1fr]"
                            : "grid grid-rows-[0fr] group-hover:grid-rows-[1fr]",
                        )}
                      >
                        <div className="overflow-hidden">
                          <p className="mt-1.5 text-xs text-text-secondary leading-relaxed">
                            {s.description}
                          </p>
                        </div>
                      </div>

                      {/* Expanded details */}
                      <AnimatePresence>
                        {isExpanded && (
                          <motion.div
                            initial={{ height: 0, opacity: 0 }}
                            animate={{ height: "auto", opacity: 1 }}
                            exit={{ height: 0, opacity: 0 }}
                            transition={{ duration: 0.2 }}
                            className="overflow-hidden"
                          >
                            {s.tools.length > 0 && (
                              <div className="mt-2.5 flex flex-wrap gap-1">
                                {s.tools.map((t) => (
                                  <span
                                    key={t}
                                    className="inline-block rounded-md bg-bg-elevated px-1.5 py-0.5 text-[10px] font-mono text-text-secondary"
                                  >
                                    {t}
                                  </span>
                                ))}
                              </div>
                            )}
                            <p className="mt-2 text-[10px] text-text-muted font-mono truncate">
                              {s.source_path}
                            </p>
                          </motion.div>
                        )}
                      </AnimatePresence>
                    </div>
                  </div>
                </motion.div>
              );
            })}
          </div>

          {filtered.length === 0 && filter && (
            <div className="text-center text-sm text-text-muted py-12">
              No skills matching "<span className="text-text">{filter}</span>"
            </div>
          )}
        </>
      )}
    </div>
  );
}
