import { ShieldAlert, TerminalSquare } from 'lucide-react'
import type { ApprovalRequest } from './types'

interface ApprovalPromptProps {
  approval: ApprovalRequest
  resolving?: 'approve' | 'deny' | null
  onApprove: () => void
  onDeny: () => void
}

export function ApprovalPrompt({ approval, resolving = null, onApprove, onDeny }: ApprovalPromptProps) {
  const busy = resolving !== null

  return (
    <div className="border-t border-border bg-amber-950/20 px-4 py-4">
      <div className="mx-auto max-w-3xl rounded-2xl border border-amber-500/30 bg-bg-surface/95 p-4 shadow-lg shadow-amber-950/10">
        <div className="flex items-start gap-3">
          <div className="rounded-xl bg-amber-500/10 p-2 text-amber-300">
            <ShieldAlert size={18} />
          </div>
          <div className="min-w-0 flex-1">
            <div className="flex flex-wrap items-center gap-2">
              <h3 className="text-sm font-semibold text-text">Approval Required</h3>
              <span className="rounded-full border border-amber-500/30 bg-amber-500/10 px-2 py-0.5 text-[11px] uppercase tracking-[0.14em] text-amber-300">
                {approval.sandbox_mode}
              </span>
            </div>
            <p className="mt-1 text-sm text-text-muted">
              <span className="font-medium text-text">{approval.tool}</span> is waiting for confirmation before it runs.
            </p>
            {approval.command && (
              <div className="mt-3 overflow-hidden rounded-xl border border-border bg-bg/80">
                <div className="flex items-center gap-2 border-b border-border px-3 py-2 text-[11px] uppercase tracking-[0.14em] text-text-muted">
                  <TerminalSquare size={14} />
                  Command Preview
                </div>
                <pre className="overflow-x-auto px-3 py-3 text-sm text-text">
                  <code>{approval.command}</code>
                </pre>
              </div>
            )}
            <div className="mt-3 rounded-xl border border-border bg-bg/50 px-3 py-3 text-sm leading-6 text-text-muted whitespace-pre-wrap">
              {approval.prompt}
            </div>
            <div className="mt-4 flex flex-wrap items-center gap-3">
              <button
                type="button"
                onClick={onApprove}
                disabled={busy}
                className="rounded-lg bg-accent px-4 py-2 text-sm font-medium text-white transition-opacity hover:opacity-90 disabled:cursor-not-allowed disabled:opacity-50"
              >
                {resolving === 'approve' ? 'Approving...' : 'Approve'}
              </button>
              <button
                type="button"
                onClick={onDeny}
                disabled={busy}
                className="rounded-lg border border-red-500/30 bg-red-500/10 px-4 py-2 text-sm font-medium text-red-300 transition-colors hover:bg-red-500/15 disabled:cursor-not-allowed disabled:opacity-50"
              >
                {resolving === 'deny' ? 'Denying...' : 'Deny'}
              </button>
            </div>
          </div>
        </div>
      </div>
    </div>
  )
}
