import { Bot, Lock, Sparkles } from 'lucide-react'
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

function originSummary(session: ChatSessionSummary | null): string | null {
  if (!session) return null
  if (session.origin_kind === 'channel') {
    return session.origin_ref || 'Channel session'
  }
  if (session.origin_kind === 'cli') {
    return 'CLI session'
  }
  if (session.origin_kind === 'web') {
    return 'Web session'
  }
  return session.origin_kind || null
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
    <div className="border-b border-border bg-bg/95 px-4 py-4 backdrop-blur">
      <div className="mx-auto flex max-w-3xl items-start justify-between gap-4">
        <div className="min-w-0">
          <div className="flex flex-wrap items-center gap-2">
            <span className="inline-flex items-center gap-2 rounded-full border border-border bg-bg-surface px-3 py-1 text-xs font-medium text-text-muted">
              <Bot size={13} className="text-accent" />
              {agent?.key ?? 'Loading agent'}
            </span>
            {session && (
              <span className="rounded-full border border-border bg-bg-surface px-3 py-1 text-xs text-text-muted">
                {originSummary(session)}
              </span>
            )}
            {readOnly && (
              <span className="inline-flex items-center gap-1 rounded-full bg-amber-500/12 px-3 py-1 text-xs font-medium text-amber-300">
                <Lock size={12} />
                Read only
              </span>
            )}
          </div>

          <div className="mt-3 flex flex-wrap items-center gap-x-3 gap-y-1">
            <h1 className="text-lg font-semibold tracking-tight text-text">
              {agent?.key ?? 'Chat'}
            </h1>
            {agent && (
              <span className="inline-flex items-center gap-1 text-sm text-text-muted">
                <Sparkles size={14} className="text-accent" />
                {agent.provider} / {agent.model}
              </span>
            )}
          </div>

          <div className="mt-1 text-sm text-text-muted">
            {agent?.workspace ? `Workspace: ${agent.workspace}` : 'Select a session or start a new one for this agent.'}
          </div>

          {readOnly && (
            <div className="mt-3 rounded-xl border border-amber-500/20 bg-amber-500/8 px-3 py-2 text-sm text-amber-100">
              <div className="font-medium text-amber-200">Channel session · read only</div>
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
