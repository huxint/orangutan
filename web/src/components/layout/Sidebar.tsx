import { useState, useEffect, useCallback } from 'react'
import { Link, useLocation, useNavigate } from 'react-router-dom'
import { MessageSquare, Settings, Wrench, Bot, Zap, Monitor, Plus, X } from 'lucide-react'
import { cn } from '../../lib/utils'
import { apiFetch } from '../../api/client'

interface Session {
  id: string
  created_at: string
  model: string
  message_count: number
  preview?: string
}

const navItems = [
  { to: '/chat', label: 'Chat', icon: MessageSquare },
  { to: '/config', label: 'Config', icon: Settings },
  { to: '/tools', label: 'Tools', icon: Wrench },
  { to: '/agents', label: 'Agents', icon: Bot },
  { to: '/skills', label: 'Skills', icon: Zap },
  { to: '/system', label: 'System', icon: Monitor },
]

function groupByDate(sessions: Session[]): { label: string; sessions: Session[] }[] {
  const now = new Date()
  const today = new Date(now.getFullYear(), now.getMonth(), now.getDate())
  const yesterday = new Date(today.getTime() - 86400000)

  const groups: Record<string, Session[]> = { Today: [], Yesterday: [], Older: [] }

  for (const s of sessions) {
    const d = new Date(s.created_at)
    const day = new Date(d.getFullYear(), d.getMonth(), d.getDate())
    if (day.getTime() === today.getTime()) groups.Today.push(s)
    else if (day.getTime() === yesterday.getTime()) groups.Yesterday.push(s)
    else groups.Older.push(s)
  }

  return ['Today', 'Yesterday', 'Older']
    .filter(label => groups[label].length > 0)
    .map(label => ({ label, sessions: groups[label] }))
}

function formatTime(iso: string): string {
  const d = new Date(iso)
  return d.toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' })
}

function truncate(s: string, max: number): string {
  return s.length > max ? s.slice(0, max) + '...' : s
}

export function Sidebar() {
  const location = useLocation()
  const navigate = useNavigate()
  const [sessions, setSessions] = useState<Session[]>([])
  const [hoveredId, setHoveredId] = useState<string | null>(null)

  const fetchSessions = useCallback(() => {
    apiFetch<Session[]>('/api/sessions')
      .then(setSessions)
      .catch(() => {})
  }, [])

  useEffect(() => {
    fetchSessions()
    const timer = setInterval(fetchSessions, 30000)
    return () => clearInterval(timer)
  }, [fetchSessions])

  const activeSessionId = location.pathname.match(/^\/chat\/(.+)/)?.[1]

  const handleDelete = useCallback(async (e: React.MouseEvent, id: string) => {
    e.preventDefault()
    e.stopPropagation()
    await apiFetch(`/api/sessions/${id}`, { method: 'DELETE' }).catch(() => {})
    setSessions(prev => prev.filter(s => s.id !== id))
    if (activeSessionId === id) navigate('/chat')
  }, [activeSessionId, navigate])

  const handleNewChat = () => {
    navigate('/chat')
  }

  const groups = groupByDate(sessions)

  return (
    <aside className="w-64 border-r border-border bg-bg-surface flex flex-col shrink-0">
      <div className="p-3">
        <button
          onClick={handleNewChat}
          className="w-full flex items-center gap-2 px-3 py-2 rounded-lg bg-accent text-white font-medium text-sm hover:bg-accent-hover transition-colors"
        >
          <Plus size={16} />
          New Chat
        </button>
      </div>

      <nav className="px-2 space-y-0.5">
        {navItems.map(({ to, label, icon: Icon }) => {
          const active = location.pathname === to || location.pathname.startsWith(to + '/')
          return (
            <Link
              key={to}
              to={to}
              className={cn(
                'flex items-center gap-2.5 px-3 py-2 rounded-lg text-sm transition-colors',
                active
                  ? 'bg-accent-bg text-accent font-medium'
                  : 'text-text-muted hover:text-text hover:bg-bg-elevated'
              )}
            >
              <Icon size={16} />
              {label}
            </Link>
          )
        })}
      </nav>

      <div className="border-t border-border mt-2 flex-1 flex flex-col min-h-0">
        <div className="px-4 pt-3 pb-1">
          <span className="text-xs font-medium text-text-muted uppercase tracking-wider">Sessions</span>
        </div>

        <div className="flex-1 overflow-y-auto px-2 pb-2">
          {groups.length === 0 ? (
            <p className="px-3 py-4 text-xs text-text-muted">No sessions yet</p>
          ) : (
            groups.map(group => (
              <div key={group.label} className="mt-2">
                <div className="px-3 py-1 text-[10px] font-semibold text-text-muted uppercase tracking-wider">
                  {group.label}
                </div>
                {group.sessions.map(s => {
                  const isActive = activeSessionId === s.id
                  return (
                    <Link
                      key={s.id}
                      to={`/chat/${s.id}`}
                      className={cn(
                        'group flex items-center justify-between px-3 py-1.5 rounded-lg text-sm transition-colors',
                        isActive
                          ? 'bg-accent-bg text-accent'
                          : 'text-text-muted hover:text-text hover:bg-bg-elevated'
                      )}
                      onMouseEnter={() => setHoveredId(s.id)}
                      onMouseLeave={() => setHoveredId(null)}
                    >
                      <div className="min-w-0 flex-1">
                        <div className="truncate text-sm">
                          {s.preview ? truncate(s.preview, 30) : 'New chat'}
                        </div>
                        <div className="flex items-center gap-2 text-[11px] text-text-muted">
                          <span>{formatTime(s.created_at)}</span>
                          <span className="truncate">{s.model?.split('/').pop()?.split('-').slice(0, 2).join('-') ?? ''}</span>
                        </div>
                      </div>
                      {hoveredId === s.id && (
                        <button
                          onClick={(e) => handleDelete(e, s.id)}
                          className="ml-1 p-0.5 rounded text-text-muted hover:text-red-400 hover:bg-red-950/30 transition-colors shrink-0"
                        >
                          <X size={14} />
                        </button>
                      )}
                    </Link>
                  )
                })}
              </div>
            ))
          )}
        </div>
      </div>
    </aside>
  )
}
