import { Bot, Lock, Sparkles } from 'lucide-react'
import type { AgentSummary } from '../../api/client'
import { AgentSessionSwitcher } from './AgentSessionSwitcher'
import { cn } from '../../lib/utils'
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
  if (!session) return 'New conversation'
  switch (session.origin_kind) {
    case 'channel': return 'Channel'
    case 'cli': return 'CLI session'
    default: return 'Web session'
  }
}

function scopeColor(session: ChatSessionSummary | null): string {
  if (!session) return 'bg-success text-success'
  switch (session.origin_kind) {
    case 'channel': return 'bg-warning text-warning'
    case 'cli': return 'bg-sky-400 text-sky-400'
    default: return 'bg-success text-success'
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
  const sc = scopeColor(session)

  return (
    <div className="shrink-0 border-b border-border/60 glass-surface">
      <div className="mx-auto flex w-full max-w-5xl items-center justify-between gap-3 px-4 py-2">
        {/* Left: agent info */}
        <div className="min-w-0 flex items-center gap-3">
          {/* Agent avatar */}
          <div className="relative shrink-0">
            <div className="p-2 rounded-xl bg-gradient-to-br from-accent/20 to-accent/5 border border-accent/10">
              <Bot size={18} className="text-accent" />
            </div>
            {/* Online pulse dot */}
            <div className={cn('absolute -bottom-0.5 -right-0.5 w-2.5 h-2.5 rounded-full border-2 border-bg-surface', sc.split(' ')[0])} />
          </div>

          <div className="min-w-0">
            <div className="flex items-center gap-2">
              <span className="text-sm font-bold text-text truncate">
                {agent?.key ?? 'Unknown'}
              </span>
              {agent?.model && (
                <span className="hidden sm:inline-flex items-center gap-1 text-[10px] text-text-muted bg-bg-elevated/80 px-1.5 py-0.5 rounded-md font-mono border border-border/50">
                  <Sparkles size={8} className="text-accent/60" />
                  {agent.model}
                </span>
              )}
            </div>
            <div className="flex items-center gap-1.5 mt-0.5">
              <div className={cn('w-1.5 h-1.5 rounded-full', sc.split(' ')[0])} />
              <span className="text-[11px] text-text-muted">
                {scopeLabel(session)}
              </span>
              {session && (
                <>
                  <span className="text-text-muted/40 text-[10px]">·</span>
                  <span className="text-[11px] text-text-muted">{session.message_count} msgs</span>
                </>
              )}
            </div>
          </div>

          {readOnly && (
            <div className="flex items-center gap-1 text-warning text-[10px] font-semibold bg-warning-dim px-2 py-1 rounded-full border border-warning/15">
              <Lock size={10} />
              Read-only
            </div>
          )}
        </div>

        {/* Right: session controls */}
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
