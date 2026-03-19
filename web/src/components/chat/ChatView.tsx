import { useState, useRef, useCallback, useEffect } from 'react'
import { useParams, useNavigate } from 'react-router-dom'
import { streamChat, apiFetch } from '../../api/client'
import { MessageList } from './MessageList'
import { ChatInput } from './ChatInput'

interface ContentBlock {
  type: string
  text?: string
  id?: string
  name?: string
  input?: object
  tool_use_id?: string
  content?: string
  is_error?: boolean
}

interface ChatMessage {
  role: 'user' | 'assistant'
  content: ContentBlock[]
}

interface ToolCall {
  id: string
  name: string
  input: object
  result?: { content: string; is_error: boolean }
}

interface SessionData {
  id: string
  messages: { role: string; content: ContentBlock[] }[]
}

export function ChatView() {
  const { sessionId: paramSessionId } = useParams<{ sessionId: string }>()
  const navigate = useNavigate()

  const [sessionId, setSessionId] = useState<string | null>(paramSessionId ?? null)
  const [messages, setMessages] = useState<ChatMessage[]>([])
  const [toolCalls, setToolCalls] = useState<Map<string, ToolCall>>(new Map())
  const [streaming, setStreaming] = useState(false)
  const [streamingText, setStreamingText] = useState('')
  const [error, setError] = useState<string | null>(null)
  const abortRef = useRef<AbortController | null>(null)

  useEffect(() => {
    if (!paramSessionId) {
      setSessionId(null)
      setMessages([])
      setToolCalls(new Map())
      return
    }
    setSessionId(paramSessionId)
    apiFetch<SessionData>(`/api/sessions/${paramSessionId}`)
      .then(data => {
        const msgs: ChatMessage[] = data.messages
          .filter(m => m.role === 'user' || m.role === 'assistant')
          .map(m => ({ role: m.role as 'user' | 'assistant', content: m.content }))
        setMessages(msgs)

        const tc = new Map<string, ToolCall>()
        for (const msg of data.messages) {
          for (const block of msg.content) {
            if (block.type === 'tool_use' && block.id) {
              tc.set(block.id, { id: block.id, name: block.name!, input: block.input! })
            }
            if (block.type === 'tool_result' && block.tool_use_id) {
              const existing = tc.get(block.tool_use_id)
              if (existing) {
                existing.result = { content: block.content ?? '', is_error: block.is_error ?? false }
              }
            }
          }
        }
        setToolCalls(tc)
      })
      .catch(() => { /* session not found */ })
  }, [paramSessionId])

  const handleSend = useCallback((text: string) => {
    setError(null)
    const userMsg: ChatMessage = { role: 'user', content: [{ type: 'text', text }] }
    setMessages(prev => [...prev, userMsg])
    setStreaming(true)
    setStreamingText('')

    const controller = new AbortController()
    abortRef.current = controller

    let accum = ''

    streamChat(sessionId, text, (type, data: any) => {
      switch (type) {
        case 'session':
          setSessionId(data.session_id)
          navigate(`/chat/${data.session_id}`, { replace: true })
          break
        case 'text':
          accum += data.text
          setStreamingText(accum)
          break
        case 'tool_start':
          setToolCalls(prev => {
            const next = new Map(prev)
            next.set(data.id, { id: data.id, name: data.name, input: data.input })
            return next
          })
          break
        case 'tool_end':
          setToolCalls(prev => {
            const next = new Map(prev)
            const tc = next.get(data.id)
            if (tc) tc.result = { content: data.content, is_error: data.is_error }
            return next
          })
          break
        case 'done':
          if (accum) {
            setMessages(prev => [...prev, { role: 'assistant', content: [{ type: 'text', text: accum }] }])
          }
          setStreamingText('')
          setStreaming(false)
          abortRef.current = null
          break
        case 'error':
          setError(data.error)
          setStreaming(false)
          abortRef.current = null
          break
      }
    }, controller.signal).catch(() => {
      setStreaming(false)
      abortRef.current = null
    })
  }, [sessionId, navigate])

  const handleAbort = useCallback(() => {
    abortRef.current?.abort()
    fetch('/api/chat/abort', { method: 'POST' }).catch(() => {})
    setStreaming(false)
    if (streamingText) {
      setMessages(prev => [...prev, { role: 'assistant', content: [{ type: 'text', text: streamingText }] }])
      setStreamingText('')
    }
    abortRef.current = null
  }, [streamingText])

  return (
    <div className="flex flex-col h-full">
      <MessageList messages={messages} toolCalls={toolCalls} streamingText={streamingText || undefined} />
      {error && (
        <div className="px-4 py-2 text-sm text-red-400 bg-red-950/30 text-center">
          {error}
        </div>
      )}
      <ChatInput onSend={handleSend} disabled={streaming} streaming={streaming} onAbort={handleAbort} />
    </div>
  )
}
