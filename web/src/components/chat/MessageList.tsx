import { useRef, useEffect } from 'react'
import { motion, AnimatePresence } from 'framer-motion'
import { MessageBubble } from './MessageBubble'
import type { ChatMessage } from './types'

interface MessageListProps {
  messages: ChatMessage[]
  onSuggest?: (text: string) => void
}

const SUGGESTIONS = [
  { emoji: '💡', text: 'Explain how this works' },
  { emoji: '🔧', text: 'Help me debug something' },
  { emoji: '📝', text: 'Write some code' },
]

function EmptyState({ onSuggest }: { onSuggest?: (text: string) => void }) {
  return (
    <div className="flex flex-1 items-center justify-center">
      <div className="flex flex-col items-center gap-8 px-4">
        <motion.div
          initial={{ opacity: 0, scale: 0.8, y: 10 }}
          animate={{ opacity: 1, scale: 1, y: 0 }}
          transition={{ type: 'spring', stiffness: 200, damping: 20 }}
          className="relative"
        >
          <img src="/assets/orangutan.png" alt="Orangutan" width={120} height={120} className="select-none" draggable={false} />
          <div className="absolute inset-0 blur-3xl bg-accent/8 rounded-full scale-150" />
        </motion.div>

        <motion.div
          initial={{ opacity: 0, y: 8 }}
          animate={{ opacity: 1, y: 0 }}
          transition={{ delay: 0.1 }}
          className="text-center"
        >
          <h2 className="text-xl font-semibold text-text mb-1.5">
            What can I help with?
          </h2>
          <p className="text-sm text-text-muted">
            Ask anything or pick a suggestion.
          </p>
        </motion.div>

        {onSuggest && (
          <motion.div
            initial={{ opacity: 0, y: 8 }}
            animate={{ opacity: 1, y: 0 }}
            transition={{ delay: 0.2 }}
            className="flex gap-2.5"
          >
            {SUGGESTIONS.map(s => (
              <button
                key={s.text}
                onClick={() => onSuggest(s.text)}
                className="rounded-xl border border-border bg-bg-surface
                  px-4 py-2.5 text-sm text-text-secondary
                  hover:border-accent/30 hover:text-text
                  transition-all duration-200 hover:-translate-y-0.5
                  hover:shadow-[0_4px_16px_rgba(0,0,0,0.15)]"
              >
                <span className="mr-1.5">{s.emoji}</span>
                {s.text}
              </button>
            ))}
          </motion.div>
        )}
      </div>
    </div>
  )
}

export function MessageList({ messages, onSuggest }: MessageListProps) {
  const bottomRef = useRef<HTMLDivElement>(null)
  const containerRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    bottomRef.current?.scrollIntoView({ behavior: 'smooth' })
  }, [messages])

  const visible = messages.filter(m => m.content.length > 0)

  if (visible.length === 0) {
    return <EmptyState onSuggest={onSuggest} />
  }

  return (
    <div ref={containerRef} className="flex-1 overflow-y-auto px-4 py-6">
      <div className="mx-auto max-w-3xl space-y-4">
        <AnimatePresence initial={false}>
          {visible.map(msg => (
            <motion.div
              key={msg.id}
              initial={{ opacity: 0, y: 8 }}
              animate={{ opacity: 1, y: 0 }}
              transition={{
                type: 'spring',
                stiffness: 300,
                damping: 25,
              }}
            >
              <MessageBubble role={msg.role} content={msg.content} />
            </motion.div>
          ))}
        </AnimatePresence>
        <div ref={bottomRef} />
      </div>
    </div>
  )
}
