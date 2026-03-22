import { useEffect, useState } from "react";
import { Link, useLocation, useNavigate } from "react-router-dom";
import {
  MessageSquare,
  Settings,
  Wrench,
  Bot,
  Zap,
  Monitor,
  Plus,
  PanelLeftClose,
  PanelLeftOpen,
  Sun,
  Moon,
} from "lucide-react";
import { cn } from "../../lib/utils";
import { getAgents, type AgentSummary } from "../../api/client";
import { AgentTree } from "./AgentTree";
import { getTheme, toggleTheme } from "../../theme";

const navItems = [
  { to: "/chat/default", label: "Chat", icon: MessageSquare },
  { to: "/config", label: "Config", icon: Settings },
  { to: "/tools", label: "Tools", icon: Wrench },
  { to: "/agents", label: "Agents", icon: Bot },
  { to: "/skills", label: "Skills", icon: Zap },
  { to: "/system", label: "System", icon: Monitor },
];

function activeNav(locationPathname: string, itemPath: string): boolean {
  if (itemPath.startsWith("/chat")) {
    return locationPathname.startsWith("/chat");
  }
  return (
    locationPathname === itemPath || locationPathname.startsWith(itemPath + "/")
  );
}

interface SidebarProps {
  collapsed: boolean;
  onToggle: () => void;
}

export function Sidebar({ collapsed, onToggle }: SidebarProps) {
  const location = useLocation();
  const navigate = useNavigate();
  const [agents, setAgents] = useState<AgentSummary[]>([]);
  const [agentsError, setAgentsError] = useState("");
  const [theme, setThemeState] = useState(getTheme);

  useEffect(() => {
    getAgents()
      .then((data) => {
        setAgents(data);
        setAgentsError("");
      })
      .catch((error) => {
        setAgents([]);
        setAgentsError(
          error instanceof Error ? error.message : "Failed to load agents",
        );
      });
  }, []);

  const currentAgentKey = location.pathname.match(/^\/chat\/([^/]+)/)?.[1];

  const handleThemeToggle = () => {
    const next = toggleTheme();
    setThemeState(next);
  };

  return (
    <aside
      className={cn(
        "relative shrink-0 flex flex-col glass-surface z-10",
        "border-r-0 rounded-r-2xl",
        collapsed ? "sidebar-collapsed" : "sidebar-expanded",
      )}
    >
      {/* Header: Logo + Collapse toggle */}
      <div className="flex items-center h-14 px-3 shrink-0">
        {!collapsed && (
          <span className="text-gradient font-bold tracking-tight text-lg animate-fade-in select-none">
            orangutan
          </span>
        )}
        <button
          onClick={onToggle}
          className={cn(
            "p-2 rounded-xl text-text-muted hover:text-text hover:bg-bg-elevated transition-all duration-200",
            collapsed ? "mx-auto" : "ml-auto",
          )}
          aria-label={collapsed ? "Expand sidebar" : "Collapse sidebar"}
        >
          {collapsed ? (
            <PanelLeftOpen size={18} />
          ) : (
            <PanelLeftClose size={18} />
          )}
        </button>
      </div>

      {/* New Chat button */}
      <div className="px-2.5 mb-2">
        <button
          onClick={() => navigate(`/chat/${currentAgentKey ?? "default"}`)}
          className={cn(
            "btn-accent flex items-center gap-2 rounded-xl py-2.5 text-sm font-medium w-full",
            collapsed ? "justify-center px-0" : "px-3",
          )}
        >
          <Plus size={16} />
          {!collapsed && <span>New Chat</span>}
        </button>
      </div>

      {/* Navigation */}
      <nav className="px-2 space-y-0.5">
        {navItems.map(({ to, label, icon: Icon }) => {
          const active = activeNav(location.pathname, to);
          return (
            <Link
              key={to}
              to={to}
              title={collapsed ? label : undefined}
              className={cn(
                "flex items-center rounded-xl transition-all duration-200",
                collapsed ? "justify-center p-2.5" : "gap-2.5 px-3 py-2.5",
                active
                  ? "bg-accent-bg text-accent font-medium shadow-sm"
                  : "text-text-muted hover:text-text hover:bg-bg-elevated",
              )}
            >
              <Icon size={18} className="shrink-0" />
              {!collapsed && (
                <span className="text-sm sidebar-content-fade">{label}</span>
              )}
            </Link>
          );
        })}
      </nav>

      {/* Agents section */}
      {!collapsed && (
        <div className="mt-3 flex-1 min-h-0 overflow-y-auto animate-fade-in">
          <div className="px-4 pt-3 pb-2">
            <span className="text-[10px] font-semibold uppercase tracking-[0.15em] text-text-muted">
              Agents
            </span>
          </div>
          <div className="px-2 pb-3">
            {agentsError ? (
              <p className="px-3 py-2 text-xs text-danger">{agentsError}</p>
            ) : agents.length === 0 ? (
              <p className="px-3 py-2 text-xs text-text-muted">
                No agents configured.
              </p>
            ) : (
              <AgentTree
                agents={agents}
                currentAgentKey={currentAgentKey}
                onSelectAgent={(agentKey) => navigate(`/chat/${agentKey}`)}
              />
            )}
          </div>
        </div>
      )}

      {/* Bottom: Theme toggle */}
      <div className="mt-auto px-2.5 py-3 border-t border-border shrink-0">
        <button
          onClick={handleThemeToggle}
          className={cn(
            "flex items-center rounded-xl p-2.5 text-text-muted hover:text-text hover:bg-bg-elevated transition-all duration-200 w-full",
            collapsed ? "justify-center" : "gap-2.5",
          )}
          aria-label="Toggle theme"
        >
          {theme === "dark" ? <Sun size={18} /> : <Moon size={18} />}
          {!collapsed && (
            <span className="text-sm sidebar-content-fade">
              {theme === "dark" ? "Light Mode" : "Dark Mode"}
            </span>
          )}
        </button>
      </div>
    </aside>
  );
}
