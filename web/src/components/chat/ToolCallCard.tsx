import { useState } from 'react'
import { Terminal, ChevronRight, Check, X, Loader2 } from 'lucide-react'
import { motion, AnimatePresence } from 'framer-motion'
import { cn } from '../../lib/utils'

interface ToolCallCardProps {
  id: string
  name: string
  input: object
  result?: { content: string; is_error: boolean }
}

function commandPreview(input: object): string | null {
  if (!('command' in input)) return null
  const { command } = input as { command?: unknown }
  return typeof command === 'string' && command.trim() ? command : null
}

export function ToolCallCard({ name, input, result }: ToolCallCardProps) {
  const running = !result
  const [open, setOpen] = useState(false)
  const preview = commandPreview(input)

  return (
    <div className={cn(
      'my-2 overflow-hidden rounded-lg border transition-colors duration-200',
      running ? 'border-accent/15 bg-bg/60' : 'border-border bg-bg/40',
    )}>
      <button
        onClick={() => setOpen(!open)}
        className="flex w-full items-center gap-2 px-3 py-2 text-left text-xs group"
      >
        <ChevronRight
          size={12}
          className={cn('shrink-0 text-text-muted transition-transform duration-150', open && 'rotate-90')}
        />

        <div className={cn(
          'shrink-0 w-5 h-5 rounded flex items-center justify-center',
          running ? 'bg-accent-dim text-accent' : result?.is_error ? 'bg-danger-dim text-danger' : 'bg-success-dim text-success',
        )}>
          {running ? <Loader2 size={11} className="animate-spin" /> : result?.is_error ? <X size={11} /> : <Check size={11} />}
        </div>

        <span className="font-semibold text-text">{name}</span>

        {preview && (
          <code className="min-w-0 truncate text-[11px] text-text-muted bg-bg/60 px-1.5 py-0.5 rounded font-mono">
            {preview}
          </code>
        )}

        <span className="ml-auto shrink-0">
          {running && (
            <span className="flex gap-0.5">
              <span className="dot" /><span className="dot" /><span className="dot" />
            </span>
          )}
          {result?.is_error && <span className="text-[10px] font-semibold text-danger">error</span>}
          {result && !result.is_error && <span className="text-[10px] font-semibold text-success">done</span>}
        </span>
      </button>

      {running && <div className="streaming-bar" />}

      <AnimatePresence>
        {open && (
          <motion.div
            initial={{ height: 0, opacity: 0 }}
            animate={{ height: 'auto', opacity: 1 }}
            exit={{ height: 0, opacity: 0 }}
            transition={{ duration: 0.2 }}
            className="overflow-hidden"
          >
            <div className="border-t border-border px-3 py-2.5 space-y-2">
              <div className="flex items-center gap-1.5 text-[10px] font-semibold uppercase tracking-widest text-text-muted">
                <Terminal size={10} /> Input
              </div>
              <pre className="overflow-x-auto rounded bg-bg-elevated border border-border px-2.5 py-2 text-[11px] text-text-secondary font-mono leading-relaxed">
                {JSON.stringify(input, null, 2)}
              </pre>
              {result && (
                <>
                  <div className="flex items-center gap-1.5 text-[10px] font-semibold uppercase tracking-widest text-text-muted">
                    {result.is_error ? <X size={10} className="text-danger" /> : <Check size={10} className="text-success" />}
                    Result
                  </div>
                  <pre className={cn(
                    'overflow-x-auto rounded bg-bg-elevated border border-border px-2.5 py-2 text-[11px] font-mono leading-relaxed',
                    result.is_error ? 'text-danger/80' : 'text-text-secondary',
                  )}>
                    {result.content}
                  </pre>
                </>
              )}
            </div>
          </motion.div>
        )}
      </AnimatePresence>
    </div>
  )
}
