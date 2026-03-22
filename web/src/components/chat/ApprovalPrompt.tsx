import { ShieldAlert, Terminal } from "lucide-react";
import { motion } from "framer-motion";
import type { ApprovalRequest } from "./types";

interface ApprovalPromptProps {
  approval: ApprovalRequest;
  resolving?: "approve" | "deny" | null;
  onApprove: () => void;
  onDeny: () => void;
}

export function ApprovalPrompt({
  approval,
  resolving = null,
  onApprove,
  onDeny,
}: ApprovalPromptProps) {
  const busy = resolving !== null;

  return (
    <div className="px-4 py-2">
      <motion.div
        initial={{ opacity: 0, y: 6, scale: 0.98 }}
        animate={{ opacity: 1, y: 0, scale: 1 }}
        className="mx-auto max-w-3xl rounded-xl border border-warning/20 bg-bg-surface p-3"
      >
        <div className="flex items-center gap-3">
          {/* Icon */}
          <div className="shrink-0 rounded-lg bg-warning-dim p-2 text-warning">
            <ShieldAlert size={16} />
          </div>

          {/* Content */}
          <div className="min-w-0 flex-1">
            <div className="flex items-center gap-2 text-sm">
              <span className="font-semibold text-text">{approval.tool}</span>
              <span className="text-[10px] font-semibold uppercase tracking-wider text-warning bg-warning-dim px-1.5 py-0.5 rounded">
                {approval.sandbox_mode}
              </span>
            </div>

            {approval.command && (
              <div className="mt-1.5 flex items-center gap-1.5 rounded-md bg-bg-elevated border border-border px-2.5 py-1.5">
                <Terminal size={11} className="text-text-muted shrink-0" />
                <code className="text-xs text-text font-mono truncate">
                  {approval.command}
                </code>
              </div>
            )}

            {approval.prompt && !approval.command && (
              <p className="mt-1 text-xs text-text-secondary line-clamp-2">
                {approval.prompt}
              </p>
            )}
          </div>

          {/* Actions */}
          <div className="shrink-0 flex items-center gap-2">
            <button
              type="button"
              onClick={onDeny}
              disabled={busy}
              className="rounded-lg border border-border bg-bg-elevated px-3 py-1.5 text-xs font-medium text-text-secondary
                hover:text-danger hover:border-danger/30 transition-colors disabled:opacity-40"
            >
              {resolving === "deny" ? "..." : "Deny"}
            </button>
            <button
              type="button"
              onClick={onApprove}
              disabled={busy}
              className="rounded-lg bg-accent px-3 py-1.5 text-xs font-semibold text-white
                hover:bg-accent-hover transition-colors disabled:opacity-40
                shadow-[0_2px_8px_rgba(249,115,22,0.2)]"
            >
              {resolving === "approve" ? "..." : "Approve"}
            </button>
          </div>
        </div>
      </motion.div>
    </div>
  );
}
