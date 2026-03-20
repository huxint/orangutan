import { useRef, useState, useCallback, type KeyboardEvent } from 'react'
import { Send, Square } from 'lucide-react'

interface ChatInputProps {
  onSend: (text: string) => void
  onQueue?: (text: string) => void
  disabled?: boolean
  onAbort?: () => void
  streaming?: boolean
  queuedMessages?: string[]
}

export function ChatInput({ onSend, onQueue, disabled, onAbort, streaming, queuedMessages = [] }: ChatInputProps) {
  const [text, setText] = useState('')
  const ref = useRef<HTMLTextAreaElement>(null)

  const resize = useCallback(() => {
    const el = ref.current
    if (!el) return
    el.style.height = 'auto'
    el.style.height = Math.min(el.scrollHeight, 200) + 'px'
  }, [])

  function clearInput() {
    setText('')
    if (ref.current) {
      ref.current.style.height = 'auto'
    }
  }

  function send() {
    const trimmed = text.trim()
    if (!trimmed || disabled) return
    onSend(trimmed)
    clearInput()
  }

  function queue() {
    const trimmed = text.trim()
    if (!trimmed || !streaming || !onQueue) return
    onQueue(trimmed)
    clearInput()
  }

  function onKey(e: KeyboardEvent) {
    if (e.key === 'Enter' && (e.ctrlKey || e.metaKey) && streaming && text.trim()) {
      e.preventDefault()
      queue()
      return
    }
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault()
      if (!streaming) {
        send()
      }
    }
  }

  return (
    <div className="border-t border-border bg-bg p-4">
      {queuedMessages.length > 0 && (
        <div className="mx-auto mb-3 max-w-3xl rounded-lg border border-border bg-bg-surface/80 p-3">
          <div className="mb-2 flex items-center justify-between text-xs text-text-muted">
            <span>Queued Messages</span>
            <span>{queuedMessages.length}</span>
          </div>
          <div className="max-h-32 space-y-2 overflow-y-auto">
            {queuedMessages.map((message, index) => (
              <div key={`${index}-${message}`} className="rounded-md bg-bg px-3 py-2 text-sm text-text">
                <span className="mr-2 text-text-muted">{index + 1}.</span>
                <span className="whitespace-pre-wrap break-words">{message}</span>
              </div>
            ))}
          </div>
        </div>
      )}
      <div className="flex items-end gap-2 max-w-3xl mx-auto">
        <textarea
          ref={ref}
          value={text}
          onChange={e => { setText(e.target.value); resize() }}
          onKeyDown={onKey}
          placeholder="Send a message..."
          rows={1}
          className="flex-1 resize-none rounded-lg border border-border bg-bg-surface px-3 py-2 text-text placeholder:text-text-muted focus:outline-none focus:border-accent"
        />
        {streaming ? (
          <button
            onClick={onAbort}
            className="shrink-0 rounded-lg bg-red-600 p-2 text-white hover:bg-red-700 transition-colors"
            title="Stop"
          >
            <Square size={18} />
          </button>
        ) : (
          <button
            onClick={send}
            disabled={disabled || !text.trim()}
            className="shrink-0 rounded-lg bg-accent p-2 text-white hover:opacity-90 transition-colors disabled:opacity-40"
            title="Send"
          >
            <Send size={18} />
          </button>
        )}
      </div>
      {streaming && (
        <div className="mx-auto mt-2 flex max-w-3xl items-center justify-between text-xs text-text-muted">
          <span>Continue typing. Press Ctrl+Enter or Cmd+Enter to queue this draft.</span>
          {queuedMessages.length > 0 && <span>{queuedMessages.length} queued</span>}
        </div>
      )}
    </div>
  )
}
