import { Bot, Lock } from 'lucide-react'
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
  if (!session) {
    return 'New conversation'
  }

  switch (session.origin_kind) {
    case 'channel':
      return 'Channel session'
    case 'cli':
      return 'CLI session'
    default:
      return 'Web session'
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
    <div className="border-b border-border bg-bg-surface/90">
      <div className="mx-auto flex w-full max-w-5xl items-start justify-between gap-4 px-4 py-4">
        <div className="min-w-0">
          <div className="flex flex-wrap items-center gap-2 text-xs text-text-muted">
            <span className="inline-flex items-center gap-1 rounded-full border border-border bg-bg px-2.5 py-1">
              <Bot size={12} />
              Agent
            </span>
            <span className="rounded-full border border-border bg-bg px-2.5 py-1 text-text">
              {agent?.key ?? 'Unknown'}
            </span>
            <span className="rounded-full border border-border bg-bg px-2.5 py-1">
              {scopeLabel(session)}
            </span>
          </div>

          <div className="mt-2 flex flex-wrap items-center gap-3">
            <h1 className="text-lg font-semibold text-text">
              {agent?.key ?? 'Agent not found'}
            </h1>
            {agent && (
              <div className="flex flex-wrap items-center gap-2 text-xs text-text-muted">
                <span>{agent.provider}</span>
                <span>{agent.model}</span>
              </div>
            )}
          </div>

          {agent?.workspace && (
            <div className="mt-1 truncate text-xs text-text-muted">
              Workspace: {agent.workspace}
            </div>
          )}

          {readOnly && (
            <div className="mt-3 rounded-xl border border-amber-500/20 bg-amber-500/8 px-4 py-3 text-sm text-amber-100">
              <div className="flex items-center gap-2 font-medium text-amber-200">
                <Lock size={14} />
                Channel session · read only
              </div>
              <div className="mt-1 text-amber-100/90">
                View history here, but continue the conversation from its original channel.
              </div>
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
