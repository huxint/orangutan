import { useEffect, useState } from "react";
import { Save, RotateCcw, CheckCircle, AlertCircle, FileJson } from "lucide-react";
import { motion, AnimatePresence } from "framer-motion";
import { apiFetch } from "../../api/client";
import { cn } from "../../lib/utils";

type ConfigDocument = Record<string, unknown>;

const editorCls = cn(
  "min-h-[26rem] w-full rounded-xl border border-border bg-bg px-4 py-3",
  "font-mono text-[12px] leading-6 text-text",
  "placeholder:text-text-muted focus:outline-none focus:ring-1 focus:ring-accent/30 focus:border-accent/40",
  "transition-all duration-150",
);

export function ConfigPage() {
  const [documentText, setDocumentText] = useState("");
  const [error, setError] = useState("");
  const [saving, setSaving] = useState(false);
  const [msg, setMsg] = useState("");

  const load = () => {
    setError("");
    apiFetch<ConfigDocument>("/api/config")
      .then((config) => setDocumentText(JSON.stringify(config, null, 2)))
      .catch((e) => setError(e.message));
  };

  useEffect(load, []);

  const save = async () => {
    setSaving(true);
    setMsg("");

    let parsed: ConfigDocument;
    try {
      parsed = JSON.parse(documentText) as ConfigDocument;
    } catch (e) {
      setMsg(`err:${e instanceof Error ? e.message : "Invalid JSON"}`);
      setSaving(false);
      return;
    }

    try {
      await apiFetch("/api/config", {
        method: "PUT",
        body: JSON.stringify(parsed),
      });
      setDocumentText(JSON.stringify(parsed, null, 2));
      setMsg("ok");
      setTimeout(() => setMsg(""), 2500);
    } catch (e: unknown) {
      setMsg(`err:${e instanceof Error ? e.message : e}`);
    } finally {
      setSaving(false);
    }
  };

  if (error) {
    return (
      <div className="p-6 space-y-3">
        <h1 className="text-xl font-bold">Configuration</h1>
        <div className="rounded-xl border border-danger/20 bg-danger-dim p-4 text-danger text-sm">
          {error}
          <button
            onClick={load}
            className="ml-3 inline-flex items-center gap-1 text-xs underline"
          >
            <RotateCcw size={12} />
            Retry
          </button>
        </div>
      </div>
    );
  }

  if (!documentText) {
    return (
      <div className="p-6">
        <h1 className="text-xl font-bold mb-3">Configuration</h1>
        <div className="flex items-center gap-2 text-sm text-text-muted">
          <div className="w-3.5 h-3.5 rounded-full border-2 border-accent border-t-transparent animate-spin" />
          Loading...
        </div>
      </div>
    );
  }

  return (
    <div className="p-6 max-w-4xl space-y-3 overflow-y-auto h-full pb-10">
      <div className="flex items-center justify-between sticky top-0 z-10 bg-bg/80 backdrop-blur-md py-2 -mt-2 -mx-1 px-1">
        <div>
          <h1 className="text-xl font-bold">Configuration</h1>
          <p className="text-xs text-text-muted mt-0.5">
            Edit the full JSON document. This preserves nested profiles, models,
            and agent mappings.
          </p>
        </div>
        <div className="flex items-center gap-2.5">
          <AnimatePresence>
            {msg && (
              <motion.span
                initial={{ opacity: 0, x: 8 }}
                animate={{ opacity: 1, x: 0 }}
                exit={{ opacity: 0, x: 8 }}
                className={cn(
                  "flex items-center gap-1 text-xs",
                  msg.startsWith("err") ? "text-danger" : "text-success",
                )}
              >
                {msg.startsWith("err") ? (
                  <AlertCircle size={12} />
                ) : (
                  <CheckCircle size={12} />
                )}
                {msg.startsWith("err") ? msg.slice(4) : "Saved"}
              </motion.span>
            )}
          </AnimatePresence>
          <button
            onClick={save}
            disabled={saving}
            className={cn(
              "rounded-lg px-4 py-1.5 text-xs font-semibold text-white flex items-center gap-1.5",
              "transition-all duration-200",
              saving
                ? "bg-accent/50"
                : "bg-accent hover:bg-accent-hover shadow-[0_2px_8px_rgba(249,115,22,0.15)] hover:shadow-[0_4px_16px_rgba(249,115,22,0.25)]",
            )}
          >
            <Save size={12} />
            {saving ? "Saving..." : "Save"}
          </button>
        </div>
      </div>

      <div className="rounded-2xl border border-border bg-bg-surface overflow-hidden">
        <div className="flex items-center gap-2 px-4 py-3 border-b border-border bg-bg-elevated/40">
          <div className="p-1.5 rounded-lg bg-sky-500/10 text-sky-400">
            <FileJson size={14} />
          </div>
          <div>
            <div className="text-sm font-semibold text-text">config.json</div>
            <div className="text-[11px] text-text-muted">
              Top-level `profiles` define endpoints. `agents.*.profile` picks one.
            </div>
          </div>
        </div>
        <div className="p-4">
          <textarea
            className={editorCls}
            spellCheck={false}
            value={documentText}
            onChange={(e) => setDocumentText(e.target.value)}
          />
        </div>
      </div>
    </div>
  );
}
