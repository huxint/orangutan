import { Link, useLocation } from 'react-router-dom'
import { MessageSquare, Settings, Wrench, Bot, Zap, Monitor, Plus } from 'lucide-react'
import { cn } from '../../lib/utils'

const navItems = [
  { to: '/chat', label: 'Chat', icon: MessageSquare },
  { to: '/config', label: 'Config', icon: Settings },
  { to: '/tools', label: 'Tools', icon: Wrench },
  { to: '/agents', label: 'Agents', icon: Bot },
  { to: '/skills', label: 'Skills', icon: Zap },
  { to: '/system', label: 'System', icon: Monitor },
]

export function Sidebar() {
  const location = useLocation()

  return (
    <aside className="w-64 border-r border-border bg-bg-surface flex flex-col shrink-0">
      <div className="p-3">
        <button className="w-full flex items-center gap-2 px-3 py-2 rounded-lg bg-accent text-white font-medium text-sm hover:bg-accent-hover transition-colors">
          <Plus size={16} />
          New Chat
        </button>
      </div>

      <nav className="flex-1 px-2 space-y-0.5">
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

      {/* Session list placeholder */}
      <div className="border-t border-border p-3 text-xs text-text-muted">
        Sessions will appear here
      </div>
    </aside>
  )
}
