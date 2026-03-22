import { useCallback, useEffect, useRef, useState } from 'react'
import { useNavigate, useParams } from 'react-router-dom'
import {
  getAgents,
  getAgentSession,
  getAgentSessions,
  submitChatApproval,
  streamChat,
  type AgentSummary,
} from '../../api/client'
import { ApprovalPrompt } from './ApprovalPrompt'
import { ChatHeader } from './ChatHeader'
import { ChatInput } from './ChatInput'
import { MessageList } from './MessageList'
import type {
  ApprovalRequest,
  ChatErrorEvent,
  ChatMessage,
  ChatSessionData,
  ChatSessionEvent,
  ChatSessionSummary,
  ChatTextEvent,
  ChatToolEndEvent,
  ChatToolStartEvent,
  ContentBlock,
  SessionMessage,
} from './types'

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

function toSessionSummary(session: ChatSessionData): ChatSessionSummary {
  const { messages: _messages, ...summary } = session
  return summary
}

function sortSessions(sessions: ChatSessionSummary[]): ChatSessionSummary[] {
  return [...sessions].sort((left, right) => new Date(right.created_at).getTime() - new Date(left.created_at).getTime())
}

export function ChatView() {
  const { agentKey: paramAgentKey, sessionId: paramSessionId } = useParams<{ agentKey: string; sessionId?: string }>()
  const agentKey = paramAgentKey ?? 'default'
  const navigate = useNavigate()

  const messageIdRef = useRef(0)
  const nextMessageId = useCallback(() => {
    messageIdRef.current += 1
    return `chat-message-${messageIdRef.current}`
  }, [])

  const normalizeMessages = useCallback((sessionMessages: SessionMessage[]): ChatMessage[] => {
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

  const [agent, setAgent] = useState<AgentSummary | null>(null)
  const [sessionId, setSessionId] = useState<string | null>(paramSessionId ?? null)
  const [sessionMeta, setSessionMeta] = useState<ChatSessionSummary | null>(null)
  const [agentSessions, setAgentSessions] = useState<ChatSessionSummary[]>([])
  const [messages, setMessages] = useState<ChatMessage[]>([])
  const [queuedMessages, setQueuedMessages] = useState<string[]>([])
  const [pendingApproval, setPendingApproval] = useState<ApprovalRequest | null>(null)
  const [resolvingApproval, setResolvingApproval] = useState<'approve' | 'deny' | null>(null)
  const [streaming, setStreaming] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const abortRef = useRef<AbortController | null>(null)
  const sessionIdRef = useRef<string | null>(paramSessionId ?? null)
  const pendingSessionIdRef = useRef<string | null>(null)
  const queuedMessagesRef = useRef<string[]>([])
  const flushQueuedMessageRef = useRef(false)
  const activeAssistantMessageIdRef = useRef<string | null>(null)

  const refreshAgentSessions = useCallback((targetAgentKey: string) => {
    return getAgentSessions(targetAgentKey).then(data => {
      setAgentSessions(sortSessions(data))
      return data
    })
  }, [])

  const loadSession = useCallback((targetAgentKey: string, targetSessionId: string) => {
    return getAgentSession(targetAgentKey, targetSessionId).then(data => {
      setSessionMeta(toSessionSummary(data))
      setMessages(normalizeMessages(data.messages))
      activeAssistantMessageIdRef.current = null
      return data
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

  const stopStreamingState = useCallback(() => {
    abortRef.current?.abort()
    abortRef.current = null
    activeAssistantMessageIdRef.current = null
    queuedMessagesRef.current = []
    flushQueuedMessageRef.current = false
    setQueuedMessages([])
    setPendingApproval(null)
    setResolvingApproval(null)
    setStreaming(false)
  }, [])

  useEffect(() => {
    sessionIdRef.current = sessionId
  }, [sessionId])

  useEffect(() => {
    queuedMessagesRef.current = queuedMessages
  }, [queuedMessages])

  useEffect(() => {
    let cancelled = false

    stopStreamingState()
    pendingSessionIdRef.current = null
    sessionIdRef.current = paramSessionId ?? null
    setSessionId(paramSessionId ?? null)
    setSessionMeta(null)
    setMessages([])
    setPendingApproval(null)
    setResolvingApproval(null)
    setError(null)

    getAgents()
      .then(agents => {
        if (cancelled) return

        const selectedAgent = agents.find(candidate => candidate.key === agentKey) ?? null
        if (!selectedAgent) {
          setAgent(null)
          setAgentSessions([])
          setError(`Agent "${agentKey}" not found`)
          return
        }

        setAgent(selectedAgent)
        return getAgentSessions(agentKey)
          .then(sessions => {
            if (cancelled) return
            setAgentSessions(sortSessions(sessions))
          })
          .catch(fetchError => {
            if (cancelled) return
            setAgentSessions([])
            setError(fetchError instanceof Error ? fetchError.message : 'Failed to load sessions')
          })
      })
      .catch(fetchError => {
        if (cancelled) return
        setAgent(null)
        setAgentSessions([])
        setError(fetchError instanceof Error ? fetchError.message : 'Failed to load agent')
      })

    return () => {
      cancelled = true
    }
  }, [agentKey, stopStreamingState])

  useEffect(() => {
    let cancelled = false

    if (!paramSessionId) {
      pendingSessionIdRef.current = null
      sessionIdRef.current = null
      setSessionId(null)
      setSessionMeta(null)
      setMessages([])
      setPendingApproval(null)
      setResolvingApproval(null)
      return () => {
        cancelled = true
      }
    }

    setSessionId(paramSessionId)
    sessionIdRef.current = paramSessionId
    if (pendingSessionIdRef.current === paramSessionId) {
      return () => {
        cancelled = true
      }
    }

    loadSession(agentKey, paramSessionId)
      .catch(loadError => {
        if (cancelled) return
        setSessionMeta(null)
        setMessages([])
        setError(loadError instanceof Error ? loadError.message : 'Failed to load session')
      })

    return () => {
      cancelled = true
    }
  }, [agentKey, paramSessionId, loadSession])

  const handleSend = useCallback((text: string) => {
    if (sessionMeta?.read_only) {
      return
    }
    if (paramSessionId && sessionMeta?.id !== paramSessionId) {
      return
    }

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
      const switchedToDifferentSession = Boolean(activeSessionId) && paramSessionId !== activeSessionId

      setStreaming(false)
      abortRef.current = null
      activeAssistantMessageIdRef.current = null
      setPendingApproval(null)
      setResolvingApproval(null)
      removeAssistantMessageIfEmpty(assistantMessageId)
      if (queuedMessagesRef.current.length > 0) {
        flushQueuedMessageRef.current = true
      }
      if (activeSessionId) {
        void refreshAgentSessions(agentKey).catch(() => {})
      }
      if ((requestWasForNewSession || switchedToDifferentSession) && activeSessionId) {
        pendingSessionIdRef.current = null
        void loadSession(agentKey, activeSessionId).catch(() => {})
      }
    }

    streamChat(agentKey, sessionIdRef.current, text, (type, data) => {
      switch (type) {
        case 'session': {
          const sessionData = data as ChatSessionEvent
          activeSessionId = sessionData.session_id
          sessionIdRef.current = sessionData.session_id
          setSessionId(sessionData.session_id)
          if (paramSessionId !== sessionData.session_id) {
            pendingSessionIdRef.current = sessionData.session_id
            navigate(`/chat/${agentKey}/${sessionData.session_id}`, { replace: requestWasForNewSession })
          }
          break
        }
        case 'text': {
          const textData = data as ChatTextEvent
          updateAssistantMessage(assistantMessageId, content => appendAssistantBlock(content, { type: 'text', text: textData.text }))
          break
        }
        case 'tool_start': {
          const toolStartData = data as ChatToolStartEvent
          updateAssistantMessage(assistantMessageId, content => appendAssistantBlock(content, {
            type: 'tool_use',
            id: toolStartData.id,
            name: toolStartData.name,
            input: toolStartData.input,
          }))
          break
        }
        case 'tool_end': {
          const toolEndData = data as ChatToolEndEvent
          updateAssistantMessage(assistantMessageId, content => appendAssistantBlock(content, {
            type: 'tool_result',
            tool_use_id: toolEndData.id,
            content: toolEndData.content,
            is_error: toolEndData.is_error,
          }))
          break
        }
        case 'approval_request':
          setPendingApproval(data as ApprovalRequest)
          setResolvingApproval(null)
          break
        case 'done':
          finalizeRequest()
          break
        case 'error': {
          const errorData = data as ChatErrorEvent
          setError(errorData.error)
          finalizeRequest()
          break
        }
      }
    }, controller.signal).catch(streamError => {
      setError(streamError instanceof Error ? streamError.message : 'Request failed')
      finalizeRequest()
    })
  }, [agentKey, loadSession, navigate, nextMessageId, paramSessionId, refreshAgentSessions, removeAssistantMessageIfEmpty, sessionMeta?.id, sessionMeta?.read_only, updateAssistantMessage])

  useEffect(() => {
    if (streaming || !flushQueuedMessageRef.current || queuedMessages.length === 0 || sessionMeta?.read_only) {
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
  }, [streaming, queuedMessages, handleSend, sessionMeta?.read_only])

  const handleQueue = useCallback((text: string) => {
    if (sessionMeta?.read_only) {
      return
    }

    setQueuedMessages(prev => {
      const next = [...prev, text]
      queuedMessagesRef.current = next
      return next
    })
  }, [sessionMeta?.read_only])

  const handleResolveApproval = useCallback((approved: boolean) => {
    const activeSessionId = sessionIdRef.current
    if (!activeSessionId || !pendingApproval) {
      return
    }

    setError(null)
    setResolvingApproval(approved ? 'approve' : 'deny')
    submitChatApproval(activeSessionId, pendingApproval.request_id, approved)
      .then(() => {
        setPendingApproval(null)
      })
      .catch(approvalError => {
        setError(approvalError instanceof Error ? approvalError.message : 'Failed to submit approval')
      })
      .finally(() => {
        setResolvingApproval(null)
      })
  }, [pendingApproval])

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
    setPendingApproval(null)
    setResolvingApproval(null)
    setStreaming(false)
  }, [removeAssistantMessageIfEmpty])

  const handleSelectSession = useCallback((targetSessionId: string) => {
    stopStreamingState()
    navigate(`/chat/${agentKey}/${targetSessionId}`)
  }, [agentKey, navigate, stopStreamingState])

  const handleStartNewChat = useCallback(() => {
    stopStreamingState()
    navigate(`/chat/${agentKey}`)
  }, [agentKey, navigate, stopStreamingState])

  const readOnly = sessionMeta?.read_only ?? false
  const sessionLoading = Boolean(paramSessionId) && sessionMeta?.id !== paramSessionId

  const handleSuggest = useCallback((text: string) => {
    if (!readOnly && !streaming) {
      handleSend(text)
    }
  }, [readOnly, streaming, handleSend])

  return (
    <div className="flex h-full flex-col">
      <ChatHeader
        agent={agent}
        session={sessionMeta}
        sessions={agentSessions}
        readOnly={readOnly}
        onSelectSession={handleSelectSession}
        onStartNewChat={handleStartNewChat}
      />
      <MessageList messages={messages} onSuggest={!readOnly && !streaming ? handleSuggest : undefined} />
      {pendingApproval && (
        <ApprovalPrompt
          approval={pendingApproval}
          resolving={resolvingApproval}
          onApprove={() => handleResolveApproval(true)}
          onDeny={() => handleResolveApproval(false)}
        />
      )}
      {error && (
        <div className="mx-4 mb-2 rounded-xl bg-danger-bg px-4 py-2.5 text-center text-sm text-danger animate-fade-in">
          {error}
        </div>
      )}
      <ChatInput
        onSend={handleSend}
        onQueue={handleQueue}
        disabled={streaming || sessionLoading}
        readOnly={readOnly}
        streaming={streaming}
        onAbort={handleAbort}
        queuedMessages={queuedMessages}
      />
    </div>
  )
}
