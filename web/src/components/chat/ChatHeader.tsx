import { Bot, Lock, Globe } from 'lucide-react'
import type { AgentSummary } from '../../api/client'
import { AgentSessionSwitcher } from './AgentSessionSwitcher'
import type { ChatSessionSummary } from './types'

interface ChatHeaderProps {
  agent: AgentSummary | null
  session: ChatSessionSummary | null
  sessions: ChatSessionSummary[]
  readOnly: boolean
  onSelectSession: (sessionId: string) => void
  onStartNewChat: () => void
}

function scopeLabel(session: ChatSessionSummary | null): string {
  if (!session) return 'New'
  switch (session.origin_kind) {
    case 'channel': return 'Channel'
    case 'cli': return 'CLI'
    default: return 'Web'
  }
}

export function ChatHeader({
  agent,
  session,
  sessions,
  readOnly,
  onSelectSession,
  onStartNewChat,
}: ChatHeaderProps) {
  return (
    <div className="shrink-0 border-b border-border bg-bg-surface/60 backdrop-blur-md">
      <div className="mx-auto flex w-full max-w-5xl items-center justify-between gap-3 px-4 py-2.5">
        <div className="min-w-0 flex items-center gap-2.5">
          <div className="shrink-0 p-1.5 rounded-lg bg-accent-dim text-accent">
            <Bot size={16} />
          </div>
          <div className="min-w-0">
            <div className="flex items-center gap-2">
              <span className="text-sm font-semibold text-text truncate">
                {agent?.key ?? 'Unknown'}
              </span>
              <span className="text-[10px] text-text-muted bg-bg-elevated px-1.5 py-0.5 rounded font-medium">
                {agent?.model ?? ''}
              </span>
            </div>
            <div className="flex items-center gap-1.5 text-[11px] text-text-muted">
              <Globe size={9} />
              <span>{scopeLabel(session)}</span>
            </div>
          </div>
          {readOnly && (
            <div className="flex items-center gap-1 text-warning text-[11px] font-medium bg-warning-dim px-2 py-0.5 rounded-full">
              <Lock size={10} />
              Read-only
            </div>
          )}
        </div>

        <AgentSessionSwitcher
          sessions={sessions}
          currentSessionId={session?.id ?? null}
          onSelectSession={onSelectSession}
          onStartNewChat={onStartNewChat}
        />
      </div>
    </div>
  )
}
