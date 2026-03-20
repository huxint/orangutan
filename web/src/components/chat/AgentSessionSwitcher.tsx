import { useEffect, useRef, useState } from 'react'
import { CalendarDays, ChevronDown, MessageSquarePlus, Radio, TerminalSquare } from 'lucide-react'
import { cn } from '../../lib/utils'
import type { ChatSessionSummary } from './types'

interface AgentSessionSwitcherProps {
  sessions: ChatSessionSummary[]
  currentSessionId?: string | null
  onSelectSession: (sessionId: string) => void
  onStartNewChat: () => void
}

interface SessionGroup {
  label: string
  sessions: ChatSessionSummary[]
}

function formatTime(iso: string): string {
  return new Date(iso).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' })
}

function formatDateLabel(iso: string): string {
  const current = new Date()
  const today = new Date(current.getFullYear(), current.getMonth(), current.getDate())
  const yesterday = new Date(today.getTime() - 86400000)
  const value = new Date(iso)
  const day = new Date(value.getFullYear(), value.getMonth(), value.getDate())

  if (day.getTime() === today.getTime()) {
    return 'Today'
  }

  if (day.getTime() === yesterday.getTime()) {
    return 'Yesterday'
  }

  return value.toLocaleDateString([], { month: 'short', day: 'numeric', year: 'numeric' })
}

function groupSessions(sessions: ChatSessionSummary[]): SessionGroup[] {
  const groups = new Map<string, ChatSessionSummary[]>()

  for (const session of sessions) {
    const label = formatDateLabel(session.created_at)
    const existing = groups.get(label)
    if (existing) {
      existing.push(session)
      continue
    }

    groups.set(label, [session])
  }

  return Array.from(groups.entries()).map(([label, items]) => ({ label, sessions: items }))
}

function sessionTitle(session: ChatSessionSummary): string {
  if (session.preview?.trim()) {
    return session.preview.trim()
  }

  if (session.origin_kind === 'channel' && session.origin_ref) {
    return session.origin_ref
  }

  if (session.origin_kind === 'cli') {
    return 'CLI conversation'
  }

  if (session.origin_kind === 'web') {
    return 'Web conversation'
  }

  return session.id
}

function originBadge(session: ChatSessionSummary) {
  switch (session.origin_kind) {
    case 'channel':
      return {
        icon: Radio,
        label: 'Channel',
        className: 'border-amber-500/20 bg-amber-500/10 text-amber-200',
      }
    case 'cli':
      return {
        icon: TerminalSquare,
        label: 'CLI',
        className: 'border-sky-500/20 bg-sky-500/10 text-sky-200',
      }
    default:
      return {
        icon: CalendarDays,
        label: 'Web',
        className: 'border-emerald-500/20 bg-emerald-500/10 text-emerald-200',
      }
  }
}

export function AgentSessionSwitcher({
  sessions,
  currentSessionId,
  onSelectSession,
  onStartNewChat,
}: AgentSessionSwitcherProps) {
  const [open, setOpen] = useState(false)
  const panelRef = useRef<HTMLDivElement>(null)

  const groups = groupSessions(sessions)
  const currentSession = sessions.find(session => session.id === currentSessionId) ?? null

  useEffect(() => {
    if (!open) {
      return
    }

    function handlePointerDown(event: MouseEvent) {
      if (!panelRef.current?.contains(event.target as Node)) {
        setOpen(false)
      }
    }

    document.addEventListener('mousedown', handlePointerDown)
    return () => {
      document.removeEventListener('mousedown', handlePointerDown)
    }
  }, [open])

  return (
    <div ref={panelRef} className="relative">
      <div className="flex items-center gap-2">
        <button
          type="button"
          onClick={onStartNewChat}
          className="inline-flex items-center gap-2 rounded-lg border border-border bg-bg px-3 py-2 text-sm text-text transition-colors hover:bg-bg-elevated"
        >
          <MessageSquarePlus size={16} />
          New Chat
        </button>
        <button
          type="button"
          onClick={() => setOpen(previous => !previous)}
          className="inline-flex min-w-56 items-center justify-between gap-3 rounded-lg border border-border bg-bg px-3 py-2 text-left text-sm text-text transition-colors hover:bg-bg-elevated"
        >
          <div className="min-w-0">
            <div className="truncate font-medium">
              {currentSession ? sessionTitle(currentSession) : 'Current agent sessions'}
            </div>
            <div className="mt-0.5 text-xs text-text-muted">
              {sessions.length === 0 ? 'No sessions yet' : `${sessions.length} sessions`}
            </div>
          </div>
          <ChevronDown size={16} className={cn('shrink-0 text-text-muted transition-transform', open && 'rotate-180')} />
        </button>
      </div>

      {open && (
        <div className="absolute right-0 z-20 mt-2 w-[28rem] overflow-hidden rounded-2xl border border-border bg-bg-surface shadow-2xl">
          <div className="border-b border-border px-4 py-3">
            <div className="text-sm font-medium text-text">Sessions</div>
            <div className="mt-1 text-xs text-text-muted">
              All conversations for the current agent, across web, CLI, and channel origins.
            </div>
          </div>

          {groups.length === 0 ? (
            <div className="px-4 py-6 text-sm text-text-muted">
              No sessions for this agent yet.
            </div>
          ) : (
            <div className="max-h-[24rem] overflow-y-auto px-2 py-2">
              {groups.map(group => (
                <div key={group.label} className="mb-3 last:mb-0">
                  <div className="px-2 py-1 text-[11px] font-semibold uppercase tracking-wider text-text-muted">
                    {group.label}
                  </div>
                  <div className="space-y-1">
                    {group.sessions.map(session => {
                      const badge = originBadge(session)
                      const BadgeIcon = badge.icon

                      return (
                        <button
                          key={session.id}
                          type="button"
                          onClick={() => {
                            setOpen(false)
                            onSelectSession(session.id)
                          }}
                          className={cn(
                            'w-full rounded-xl border px-3 py-3 text-left transition-colors',
                            session.id === currentSessionId
                              ? 'border-accent/30 bg-accent-bg'
                              : 'border-transparent hover:border-border hover:bg-bg',
                          )}
                        >
                          <div className="flex items-start justify-between gap-3">
                            <div className="min-w-0 flex-1">
                              <div className="truncate text-sm font-medium text-text">
                                {sessionTitle(session)}
                              </div>
                              <div className="mt-1 flex flex-wrap items-center gap-2 text-xs text-text-muted">
                                <span>{formatTime(session.created_at)}</span>
                                <span>{session.model}</span>
                                <span>{session.message_count} messages</span>
                              </div>
                            </div>
                            <div className="shrink-0 text-xs text-text-muted">
                              {session.read_only ? 'Read only' : 'Writable'}
                            </div>
                          </div>

                          <div className="mt-2 flex flex-wrap items-center gap-2">
                            <span className={cn('inline-flex items-center gap-1 rounded-full border px-2 py-0.5 text-[11px]', badge.className)}>
                              <BadgeIcon size={12} />
                              {badge.label}
                            </span>
                            {session.origin_ref && (
                              <span className="rounded-full border border-border bg-bg px-2 py-0.5 text-[11px] text-text-muted">
                                {session.origin_ref}
                              </span>
                            )}
                          </div>
                        </button>
                      )
                    })}
                  </div>
                </div>
              ))}
            </div>
          )}
        </div>
      )}
    </div>
  )
}
