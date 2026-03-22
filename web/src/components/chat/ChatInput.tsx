import { useRef, useState, useCallback, type KeyboardEvent } from 'react'
import { ArrowUp, Square } from 'lucide-react'
import { cn } from '../../lib/utils'

interface ChatInputProps {
  onSend: (text: string) => void
  onQueue?: (text: string) => void
  disabled?: boolean
  readOnly?: boolean
  placeholder?: string
  onAbort?: () => void
  streaming?: boolean
  queuedMessages?: string[]
}

export function ChatInput({
  onSend,
  onQueue,
  disabled,
  readOnly,
  placeholder = 'Send a message or /help...',
  onAbort,
  streaming,
  queuedMessages = [],
}: ChatInputProps) {
  const [text, setText] = useState('')
  const ref = useRef<HTMLTextAreaElement>(null)

  const resize = useCallback(() => {
    const el = ref.current
    if (!el) return
    el.style.height = 'auto'
    el.style.height = Math.min(el.scrollHeight, 180) + 'px'
  }, [])

  function clearInput() {
    setText('')
    if (ref.current) ref.current.style.height = 'auto'
  }

  function send() {
    const trimmed = text.trim()
    if (!trimmed || disabled || readOnly) return
    onSend(trimmed)
    clearInput()
  }

  function queue() {
    const trimmed = text.trim()
    if (!trimmed || !streaming || !onQueue || readOnly) return
    onQueue(trimmed)
    clearInput()
  }

  function onKey(e: KeyboardEvent) {
    if (readOnly) return
    if (e.key === 'Enter' && (e.ctrlKey || e.metaKey) && streaming && text.trim()) {
      e.preventDefault()
      queue()
      return
    }
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault()
      if (!streaming) send()
    }
  }

  const hasText = text.trim().length > 0

  return (
    <div className="px-4 pb-5 pt-2">
      {/* Queued messages */}
      {queuedMessages.length > 0 && (
        <div className="mx-auto mb-2 max-w-3xl rounded-lg border border-border bg-bg-surface p-2.5 anim-fade-up">
          <div className="flex items-center justify-between text-[11px] text-text-muted mb-1.5">
            <span>Queued</span>
            <span className="bg-accent-dim text-accent px-1.5 py-0.5 rounded text-[10px] font-semibold">
              {queuedMessages.length}
            </span>
          </div>
          <div className="max-h-24 space-y-1 overflow-y-auto">
            {queuedMessages.map((msg, i) => (
              <div key={`${i}-${msg}`} className="rounded bg-bg/50 px-2.5 py-1.5 text-xs text-text-secondary truncate">
                {msg}
              </div>
            ))}
          </div>
        </div>
      )}

      {/* Streaming bar */}
      {streaming && (
        <div className="mx-auto max-w-3xl mb-2">
          <div className="streaming-bar rounded-full" />
        </div>
      )}

      {/* Input */}
      <div className="mx-auto max-w-3xl">
        <div className={cn(
          'flex items-end gap-2 rounded-2xl border bg-bg-surface p-2',
          'transition-all duration-200',
          streaming ? 'border-accent/20' : 'border-border',
        )}>
          <textarea
            ref={ref}
            value={text}
            onChange={e => { setText(e.target.value); resize() }}
            onKeyDown={onKey}
            placeholder={readOnly ? 'Read-only session' : placeholder}
            rows={1}
            disabled={readOnly}
            className="flex-1 resize-none bg-transparent px-2 py-1.5
              text-[14.5px] text-text placeholder:text-text-muted
              focus:outline-none disabled:opacity-40 leading-relaxed"
          />
          {streaming ? (
            <button
              onClick={onAbort}
              className="shrink-0 rounded-xl bg-danger-dim text-danger p-2
                hover:bg-danger/20 transition-colors"
            >
              <Square size={16} />
            </button>
          ) : (
            <button
              onClick={send}
              disabled={disabled || readOnly || !hasText}
              className={cn(
                'shrink-0 rounded-xl p-2 transition-all duration-200',
                hasText && !disabled && !readOnly
                  ? 'bg-accent text-white shadow-[0_2px_8px_rgba(249,115,22,0.2)] hover:shadow-[0_4px_16px_rgba(249,115,22,0.3)] hover:-translate-y-px'
                  : 'bg-bg-elevated text-text-muted cursor-not-allowed',
              )}
            >
              <ArrowUp size={16} />
            </button>
          )}
        </div>
      </div>

      {/* Hint */}
      {!streaming && !readOnly && (
        <div className="mx-auto mt-1.5 max-w-3xl text-center text-[11px] text-text-muted anim-fade-in">
          <kbd className="px-1 py-0.5 rounded bg-bg-elevated text-[10px] font-mono">/help</kbd> for commands
        </div>
      )}
      {streaming && !readOnly && (
        <div className="mx-auto mt-1.5 max-w-3xl text-center text-[11px] text-text-muted anim-fade-in">
          <kbd className="px-1 py-0.5 rounded bg-bg-elevated text-[10px] font-mono">Ctrl+Enter</kbd> to queue
        </div>
      )}
    </div>
  )
}
