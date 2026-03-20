import { useEffect, useState } from 'react'
import { Link, useLocation, useNavigate } from 'react-router-dom'
import { MessageSquare, Settings, Wrench, Bot, Zap, Monitor, Plus } from 'lucide-react'
import { cn } from '../../lib/utils'
import { getAgents, type AgentSummary } from '../../api/client'
import { AgentTree } from './AgentTree'

const navItems = [
  { to: '/chat/default', label: 'Chat', icon: MessageSquare },
  { to: '/config', label: 'Config', icon: Settings },
  { to: '/tools', label: 'Tools', icon: Wrench },
  { to: '/agents', label: 'Agents', icon: Bot },
  { to: '/skills', label: 'Skills', icon: Zap },
  { to: '/system', label: 'System', icon: Monitor },
]

function activeNav(locationPathname: string, itemPath: string): boolean {
  if (itemPath.startsWith('/chat')) {
    return locationPathname.startsWith('/chat')
  }
  return locationPathname === itemPath || locationPathname.startsWith(itemPath + '/')
}

export function Sidebar() {
  const location = useLocation()
  const navigate = useNavigate()
  const [agents, setAgents] = useState<AgentSummary[]>([])
  const [agentsError, setAgentsError] = useState('')

  useEffect(() => {
    getAgents()
      .then(data => {
        setAgents(data)
        setAgentsError('')
      })
      .catch(error => {
        setAgents([])
        setAgentsError(error instanceof Error ? error.message : 'Failed to load agents')
      })
  }, [])

  const currentAgentKey = location.pathname.match(/^\/chat\/([^/]+)/)?.[1]

  return (
    <aside className="w-64 shrink-0 border-r border-border bg-bg-surface flex flex-col">
      <div className="p-3">
        <button
          onClick={() => navigate(`/chat/${currentAgentKey ?? 'default'}`)}
          className="w-full flex items-center gap-2 rounded-lg bg-accent px-3 py-2 text-sm font-medium text-white transition-colors hover:bg-accent-hover"
        >
          <Plus size={16} />
          New Chat
        </button>
      </div>

      <nav className="px-2 space-y-0.5">
        {navItems.map(({ to, label, icon: Icon }) => {
          const active = activeNav(location.pathname, to)
          return (
            <Link
              key={to}
              to={to}
              className={cn(
                'flex items-center gap-2.5 rounded-lg px-3 py-2 text-sm transition-colors',
                active
                  ? 'bg-accent-bg text-accent font-medium'
                  : 'text-text-muted hover:text-text hover:bg-bg-elevated',
              )}
            >
              <Icon size={16} />
              {label}
            </Link>
          )
        })}
      </nav>

      <div className="mt-2 flex-1 min-h-0 border-t border-border">
        <div className="px-4 pt-3 pb-2">
          <span className="text-xs font-medium uppercase tracking-wider text-text-muted">Agents</span>
        </div>

        <div className="px-2 pb-3">
          {agentsError ? (
            <p className="px-3 py-2 text-xs text-red-400">{agentsError}</p>
          ) : agents.length === 0 ? (
            <p className="px-3 py-2 text-xs text-text-muted">No agents configured.</p>
          ) : (
            <AgentTree agents={agents} currentAgentKey={currentAgentKey} onSelectAgent={agentKey => navigate(`/chat/${agentKey}`)} />
          )}
        </div>
      </div>
    </aside>
  )
}
