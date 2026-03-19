import { useRef, useState, useCallback, type KeyboardEvent } from 'react'
import { Send, Square } from 'lucide-react'

interface ChatInputProps {
  onSend: (text: string) => void
  disabled?: boolean
  onAbort?: () => void
  streaming?: boolean
}

export function ChatInput({ onSend, disabled, onAbort, streaming }: ChatInputProps) {
  const [text, setText] = useState('')
  const ref = useRef<HTMLTextAreaElement>(null)

  const resize = useCallback(() => {
    const el = ref.current
    if (!el) return
    el.style.height = 'auto'
    el.style.height = Math.min(el.scrollHeight, 200) + 'px'
  }, [])

  function send() {
    const trimmed = text.trim()
    if (!trimmed || disabled) return
    onSend(trimmed)
    setText('')
    if (ref.current) {
      ref.current.style.height = 'auto'
    }
  }

  function onKey(e: KeyboardEvent) {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault()
      send()
    }
  }

  return (
    <div className="border-t border-border bg-bg p-4">
      <div className="flex items-end gap-2 max-w-3xl mx-auto">
        <textarea
          ref={ref}
          value={text}
          onChange={e => { setText(e.target.value); resize() }}
          onKeyDown={onKey}
          placeholder="Send a message..."
          disabled={disabled}
          rows={1}
          className="flex-1 resize-none rounded-lg border border-border bg-bg-surface px-3 py-2 text-text placeholder:text-text-muted focus:outline-none focus:border-accent disabled:opacity-50"
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
    </div>
  )
}
