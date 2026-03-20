import { useRef, useEffect } from 'react'
import { MessageBubble } from './MessageBubble'
import type { ChatMessage } from './types'

interface MessageListProps {
  messages: ChatMessage[]
}

export function MessageList({ messages }: MessageListProps) {
  const bottomRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    bottomRef.current?.scrollIntoView({ behavior: 'smooth' })
  }, [messages])

  const visibleMessages = messages.filter(message => message.content.length > 0)

  if (visibleMessages.length === 0) {
    return (
      <div className="flex flex-1 items-center justify-center text-text-muted">
        Start a conversation
      </div>
    )
  }

  return (
    <div className="flex-1 overflow-y-auto p-4">
      <div className="mx-auto max-w-3xl space-y-4">
        {visibleMessages.map(message => (
          <MessageBubble key={message.id} role={message.role} content={message.content} />
        ))}
        <div ref={bottomRef} />
      </div>
    </div>
  )
}
