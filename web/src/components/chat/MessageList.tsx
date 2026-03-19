import { useRef, useEffect } from 'react'
import { MessageBubble } from './MessageBubble'
import { ToolCallCard } from './ToolCallCard'

interface ChatMessage {
  role: 'user' | 'assistant'
  content: { type: string; text?: string; id?: string; name?: string; input?: object; tool_use_id?: string; content?: string; is_error?: boolean }[]
}

interface ToolCall {
  id: string
  name: string
  input: object
  result?: { content: string; is_error: boolean }
}

interface MessageListProps {
  messages: ChatMessage[]
  toolCalls: Map<string, ToolCall>
  streamingText?: string
}

export function MessageList({ messages, toolCalls, streamingText }: MessageListProps) {
  const bottomRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    bottomRef.current?.scrollIntoView({ behavior: 'smooth' })
  }, [messages, streamingText, toolCalls.size])

  if (messages.length === 0 && !streamingText) {
    return (
      <div className="flex-1 flex items-center justify-center text-text-muted">
        Start a conversation
      </div>
    )
  }

  return (
    <div className="flex-1 overflow-y-auto p-4">
      <div className="max-w-3xl mx-auto space-y-4">
        {messages.map((msg, i) => {
          const textParts = msg.content.filter(b => b.type === 'text' && b.text).map(b => b.text!).join('')
          const toolUses = msg.content.filter(b => b.type === 'tool_use')
          const toolResults = msg.content.filter(b => b.type === 'tool_result')

          return (
            <div key={i}>
              {textParts && <MessageBubble role={msg.role} text={textParts} />}
              {toolUses.map(t => {
                const tc = toolCalls.get(t.id!) ?? { id: t.id!, name: t.name!, input: t.input! }
                return <ToolCallCard key={t.id} {...tc} />
              })}
              {toolResults.map((t, j) => {
                const tc = toolCalls.get(t.tool_use_id!)
                if (tc) return null // already shown via tool_use
                return (
                  <ToolCallCard
                    key={`result-${j}`}
                    id={t.tool_use_id!}
                    name="tool"
                    input={{}}
                    result={{ content: t.content ?? '', is_error: t.is_error ?? false }}
                  />
                )
              })}
            </div>
          )
        })}
        {streamingText && <MessageBubble role="assistant" text={streamingText} />}
        <div ref={bottomRef} />
      </div>
    </div>
  )
}
