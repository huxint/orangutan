import { useEffect, useRef, useState } from 'react'
import { CalendarClock, ChevronDown, MessageSquareText, Plus } from 'lucide-react'
import type { ChatSessionSummary } from './types'
import { cn } from '../../lib/utils'

interface AgentSessionSwitcherProps {
  sessions: ChatSessionSummary[]
  currentSessionId: string | null
  onSelectSession: (sessionId: string) => void
  onStartNewChat: () => void
}

interface SessionGroup {
  label: string
  sessions: ChatSessionSummary[]
}

function formatGroupLabel(iso: string): string {
  const date = new Date(iso)
  const today = new Date()
  const startOfToday = new Date(today.getFullYear(), today.getMonth(), today.getDate())
  const startOfTarget = new Date(date.getFullYear(), date.getMonth(), date.getDate())
  const diffDays = Math.round((startOfToday.getTime() - startOfTarget.getTime()) / 86400000)

  if (diffDays === 0) return 'Today'
  if (diffDays === 1) return 'Yesterday'
  return date.toLocaleDateString([], { month: 'short', day: 'numeric', year: date.getFullYear() === today.getFullYear() ? undefined : 'numeric' })
}

function groupSessionsByDate(sessions: ChatSessionSummary[]): SessionGroup[] {
  const groups = new Map<string, ChatSessionSummary[]>()

  for (const session of sessions) {
    const label = formatGroupLabel(session.created_at)
    const existing = groups.get(label) ?? []
    existing.push(session)
    groups.set(label, existing)
  }

  return Array.from(groups.entries()).map(([label, groupedSessions]) => ({
    label,
    sessions: groupedSessions,
  }))
}

function formatTime(iso: string): string {
  return new Date(iso).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' })
}

function originLabel(session: ChatSessionSummary): string {
  if (session.origin_kind === 'channel') {
    return session.origin_ref || 'Channel'
  }
  if (session.origin_kind === 'cli') {
    return 'CLI'
  }
  if (session.origin_kind === 'web') {
    return 'Web'
  }
  return session.origin_kind || 'Session'
}

function previewLabel(session: ChatSessionSummary): string {
  if (session.preview && session.preview.trim()) {
    return session.preview
  }
  if (session.origin_kind === 'channel') {
    return 'Channel conversation'
  }
  if (session.origin_kind === 'cli') {
    return 'CLI conversation'
  }
  if (session.origin_kind === 'web') {
    return 'Web conversation'
  }
  return 'Conversation'
}

export function AgentSessionSwitcher({
  sessions,
  currentSessionId,
  onSelectSession,
  onStartNewChat,
}: AgentSessionSwitcherProps) {
  const [open, setOpen] = useState(false)
  const containerRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    if (!open) return

    function handlePointerDown(event: MouseEvent) {
      if (!containerRef.current?.contains(event.target as Node)) {
        setOpen(false)
      }
    }

    document.addEventListener('mousedown', handlePointerDown)
    return () => document.removeEventListener('mousedown', handlePointerDown)
  }, [open])

  const groups = groupSessionsByDate(sessions)

  return (
    <div ref={containerRef} className="relative">
      <button
        type="button"
        onClick={() => setOpen(value => !value)}
        className="inline-flex items-center gap-2 rounded-lg border border-border bg-bg-surface px-3 py-2 text-sm text-text transition-colors hover:bg-bg-elevated"
      >
        <CalendarClock size={15} className="text-accent" />
        <span>Sessions</span>
        <span className="rounded-full bg-bg px-2 py-0.5 text-xs text-text-muted">{sessions.length}</span>
        <ChevronDown size={14} className={cn('text-text-muted transition-transform', open && 'rotate-180')} />
      </button>

      {open && (
        <div className="absolute right-0 top-[calc(100%+0.5rem)] z-20 w-[min(26rem,calc(100vw-2rem))] overflow-hidden rounded-2xl border border-border bg-bg shadow-lg">
          <div className="flex items-center justify-between border-b border-border px-4 py-3">
            <div>
              <div className="text-sm font-medium text-text">Agent Sessions</div>
              <div className="text-xs text-text-muted">All sessions for the current agent</div>
            </div>
            <button
              type="button"
              onClick={() => {
                setOpen(false)
                onStartNewChat()
              }}
              className="inline-flex items-center gap-1.5 rounded-lg bg-accent px-3 py-2 text-xs font-medium text-white transition-opacity hover:opacity-90"
            >
              <Plus size={14} />
              New Chat
            </button>
          </div>

          <div className="max-h-[26rem] overflow-y-auto p-2">
            {groups.length === 0 ? (
              <div className="rounded-xl border border-dashed border-border bg-bg-surface/60 px-4 py-8 text-center">
                <MessageSquareText className="mx-auto mb-3 text-text-muted" size={20} />
                <div className="text-sm font-medium text-text">No sessions yet</div>
                <div className="mt-1 text-xs text-text-muted">Start a new chat for this agent.</div>
              </div>
            ) : (
              groups.map(group => (
                <section key={group.label} className="mb-3 last:mb-0">
                  <div className="px-2 pb-1 text-[11px] font-medium uppercase tracking-[0.18em] text-text-muted">
                    {group.label}
                  </div>
                  <div className="space-y-1">
                    {group.sessions.map(session => {
                      const active = currentSessionId === session.id
                      return (
                        <button
                          key={session.id}
                          type="button"
                          onClick={() => {
                            setOpen(false)
                            onSelectSession(session.id)
                          }}
                          className={cn(
                            'block w-full rounded-xl border px-3 py-2.5 text-left transition-colors',
                            active
                              ? 'border-accent/40 bg-accent-bg'
                              : 'border-transparent bg-bg-surface/70 hover:border-border hover:bg-bg-surface',
                          )}
                        >
                          <div className="flex items-start justify-between gap-3">
                            <div className="min-w-0 flex-1">
                              <div className="truncate text-sm font-medium text-text">
                                {previewLabel(session)}
                              </div>
                              <div className="mt-1 flex flex-wrap items-center gap-2 text-[11px] text-text-muted">
                                <span>{formatTime(session.created_at)}</span>
                                <span>{originLabel(session)}</span>
                                <span>{session.model}</span>
                              </div>
                            </div>
                            {session.read_only && (
                              <span className="shrink-0 rounded-full bg-amber-500/12 px-2 py-0.5 text-[11px] font-medium text-amber-300">
                                Read only
                              </span>
                            )}
                          </div>
                        </button>
                      )
                    })}
                  </div>
                </section>
              ))
            )}
          </div>
        </div>
      )}
    </div>
  )
}
