import { useState, useRef, useCallback, useEffect } from 'react'
import { useParams, useNavigate } from 'react-router-dom'
import { streamChat, apiFetch } from '../../api/client'
import { MessageList } from './MessageList'
import { ChatInput } from './ChatInput'
import type { ChatMessage, ContentBlock } from './types'

interface SessionData {
  id: string
  messages: { role: string; content: ContentBlock[] }[]
}

function appendAssistantBlock(content: ContentBlock[], block: ContentBlock): ContentBlock[] {
  if (block.type === 'text' && block.text) {
    const last = content.at(-1)
    if (last?.type === 'text') {
      return [...content.slice(0, -1), { ...last, text: (last.text ?? '') + block.text }]
    }
  }

  if (block.type === 'tool_use' && block.id) {
    const existingIndex = content.findIndex(item => item.type === 'tool_use' && item.id === block.id)
    if (existingIndex >= 0) {
      const next = [...content]
      next[existingIndex] = block
      return next
    }
  }

  if (block.type === 'tool_result' && block.tool_use_id) {
    const existingIndex = content.findIndex(item => item.type === 'tool_result' && item.tool_use_id === block.tool_use_id)
    if (existingIndex >= 0) {
      const next = [...content]
      next[existingIndex] = block
      return next
    }
  }

  return [...content, block]
}

export function ChatView() {
  const { sessionId: paramSessionId } = useParams<{ sessionId: string }>()
  const navigate = useNavigate()

  const messageIdRef = useRef(0)
  const nextMessageId = useCallback(() => {
    messageIdRef.current += 1
    return `chat-message-${messageIdRef.current}`
  }, [])

  const normalizeMessages = useCallback((sessionMessages: SessionData['messages']): ChatMessage[] => {
    const normalized: ChatMessage[] = []
    let currentAssistantId: string | null = null

    for (const message of sessionMessages) {
      if (message.role === 'assistant') {
        if (!currentAssistantId) {
          currentAssistantId = nextMessageId()
          normalized.push({ id: currentAssistantId, role: 'assistant', content: [] })
        }

        normalized[normalized.length - 1] = {
          ...normalized[normalized.length - 1],
          content: message.content.reduce(appendAssistantBlock, normalized[normalized.length - 1].content),
        }
        continue
      }

      const userTextBlocks = message.content.filter(block => block.type === 'text' && block.text)
      const toolResultBlocks = message.content.filter(block => block.type === 'tool_result' && block.tool_use_id)

      if (toolResultBlocks.length > 0) {
        if (!currentAssistantId) {
          currentAssistantId = nextMessageId()
          normalized.push({ id: currentAssistantId, role: 'assistant', content: [] })
        }

        normalized[normalized.length - 1] = {
          ...normalized[normalized.length - 1],
          content: toolResultBlocks.reduce(appendAssistantBlock, normalized[normalized.length - 1].content),
        }
      }

      if (userTextBlocks.length > 0) {
        currentAssistantId = null
        normalized.push({ id: nextMessageId(), role: 'user', content: userTextBlocks })
      }
    }

    return normalized.filter(message => message.content.length > 0)
  }, [nextMessageId])

  const [sessionId, setSessionId] = useState<string | null>(paramSessionId ?? null)
  const [messages, setMessages] = useState<ChatMessage[]>([])
  const [queuedMessages, setQueuedMessages] = useState<string[]>([])
  const [streaming, setStreaming] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const abortRef = useRef<AbortController | null>(null)
  const sessionIdRef = useRef<string | null>(paramSessionId ?? null)
  const pendingSessionIdRef = useRef<string | null>(null)
  const queuedMessagesRef = useRef<string[]>([])
  const flushQueuedMessageRef = useRef(false)
  const activeAssistantMessageIdRef = useRef<string | null>(null)

  const loadSession = useCallback((id: string) => {
    return apiFetch<SessionData>(`/api/sessions/${id}`)
      .then(data => {
        setMessages(normalizeMessages(data.messages))
        activeAssistantMessageIdRef.current = null
      })
  }, [normalizeMessages])

  const updateAssistantMessage = useCallback((messageId: string, updater: (content: ContentBlock[]) => ContentBlock[]) => {
    setMessages(prev => prev.map(message => (
      message.id === messageId
        ? { ...message, content: updater(message.content) }
        : message
    )))
  }, [])

  const removeAssistantMessageIfEmpty = useCallback((messageId: string) => {
    setMessages(prev => prev.filter(message => message.id !== messageId || message.content.length > 0))
  }, [])

  useEffect(() => {
    sessionIdRef.current = sessionId
  }, [sessionId])

  useEffect(() => {
    queuedMessagesRef.current = queuedMessages
  }, [queuedMessages])

  useEffect(() => {
    if (!paramSessionId) {
      pendingSessionIdRef.current = null
      sessionIdRef.current = null
      queuedMessagesRef.current = []
      flushQueuedMessageRef.current = false
      activeAssistantMessageIdRef.current = null
      setSessionId(null)
      setMessages([])
      setQueuedMessages([])
      setStreaming(false)
      setError(null)
      return
    }

    setSessionId(paramSessionId)
    sessionIdRef.current = paramSessionId
    if (pendingSessionIdRef.current === paramSessionId) {
      return
    }

    loadSession(paramSessionId)
      .catch(() => { /* session not found */ })
  }, [paramSessionId, loadSession])

  const handleSend = useCallback((text: string) => {
    setError(null)

    const assistantMessageId = nextMessageId()
    activeAssistantMessageIdRef.current = assistantMessageId

    setMessages(prev => [
      ...prev,
      { id: nextMessageId(), role: 'user', content: [{ type: 'text', text }] },
      { id: assistantMessageId, role: 'assistant', content: [] },
    ])
    setStreaming(true)

    const controller = new AbortController()
    abortRef.current = controller

    const requestWasForNewSession = !sessionIdRef.current
    let activeSessionId = sessionIdRef.current

    const finalizeRequest = () => {
      setStreaming(false)
      abortRef.current = null
      activeAssistantMessageIdRef.current = null
      removeAssistantMessageIfEmpty(assistantMessageId)
      if (queuedMessagesRef.current.length > 0) {
        flushQueuedMessageRef.current = true
      }
      if (requestWasForNewSession && activeSessionId) {
        pendingSessionIdRef.current = null
        void loadSession(activeSessionId).catch(() => {})
      }
    }

    streamChat(sessionIdRef.current, text, (type, data: any) => {
      switch (type) {
        case 'session':
          activeSessionId = data.session_id
          sessionIdRef.current = data.session_id
          setSessionId(data.session_id)
          if (requestWasForNewSession) {
            pendingSessionIdRef.current = data.session_id
            navigate(`/chat/${data.session_id}`, { replace: true })
          }
          break
        case 'text':
          updateAssistantMessage(assistantMessageId, content => appendAssistantBlock(content, { type: 'text', text: data.text }))
          break
        case 'tool_start':
          updateAssistantMessage(assistantMessageId, content => appendAssistantBlock(content, {
            type: 'tool_use',
            id: data.id,
            name: data.name,
            input: data.input,
          }))
          break
        case 'tool_end':
          updateAssistantMessage(assistantMessageId, content => appendAssistantBlock(content, {
            type: 'tool_result',
            tool_use_id: data.id,
            content: data.content,
            is_error: data.is_error,
          }))
          break
        case 'done':
          finalizeRequest()
          break
        case 'error':
          setError(data.error)
          finalizeRequest()
          break
      }
    }, controller.signal).catch(err => {
      setError(err instanceof Error ? err.message : 'Request failed')
      finalizeRequest()
    })
  }, [loadSession, navigate, nextMessageId, removeAssistantMessageIfEmpty, updateAssistantMessage])

  useEffect(() => {
    if (streaming || !flushQueuedMessageRef.current || queuedMessages.length === 0) {
      if (!streaming && queuedMessages.length === 0) {
        flushQueuedMessageRef.current = false
      }
      return
    }

    flushQueuedMessageRef.current = false
    const [nextMessage, ...rest] = queuedMessages
    queuedMessagesRef.current = rest
    setQueuedMessages(rest)
    handleSend(nextMessage)
  }, [streaming, queuedMessages, handleSend])

  const handleQueue = useCallback((text: string) => {
    setQueuedMessages(prev => {
      const next = [...prev, text]
      queuedMessagesRef.current = next
      return next
    })
  }, [])

  const handleAbort = useCallback(() => {
    abortRef.current?.abort()

    const activeSessionId = sessionIdRef.current
    if (activeSessionId) {
      fetch('/api/chat/abort', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ session_id: activeSessionId }),
      }).catch(() => {})
    }

    const activeAssistantMessageId = activeAssistantMessageIdRef.current
    if (activeAssistantMessageId) {
      removeAssistantMessageIfEmpty(activeAssistantMessageId)
    }

    if (queuedMessagesRef.current.length > 0) {
      flushQueuedMessageRef.current = true
    }

    activeAssistantMessageIdRef.current = null
    abortRef.current = null
    setStreaming(false)
  }, [removeAssistantMessageIfEmpty])

  return (
    <div className="flex h-full flex-col">
      <MessageList messages={messages} />
      {error && (
        <div className="bg-red-950/30 px-4 py-2 text-center text-sm text-red-400">
          {error}
        </div>
      )}
      <ChatInput
        onSend={handleSend}
        onQueue={handleQueue}
        disabled={streaming}
        streaming={streaming}
        onAbort={handleAbort}
        queuedMessages={queuedMessages}
      />
    </div>
  )
}
