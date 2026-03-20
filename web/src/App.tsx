import { HashRouter, Routes, Route, Navigate, useLocation } from 'react-router-dom'
import { SideDock } from './components/layout/SideDock'
import { ThemeToggle } from './components/layout/ThemeToggle'
import { PageTransition } from './components/layout/PageTransition'
import { ChatView } from './components/chat/ChatView'
import { ConfigPage } from './components/admin/ConfigPage'
import { ToolsPage } from './components/admin/ToolsPage'
import { AgentsPage } from './components/admin/AgentsPage'
import { SkillsPage } from './components/admin/SkillsPage'
import { SystemPage } from './components/admin/SystemPage'

function AppRoutes() {
  const location = useLocation()
  const routeKey = location.pathname.startsWith('/chat') ? 'chat' : location.pathname

  return (
    <PageTransition key={routeKey}>
      <Routes location={location}>
        <Route path="/" element={<Navigate to="/chat/default" replace />} />
        <Route path="/chat" element={<Navigate to="/chat/default" replace />} />
        <Route path="/chat/:agentKey" element={<ChatView />} />
        <Route path="/chat/:agentKey/:sessionId" element={<ChatView />} />
        <Route path="/config" element={<ConfigPage />} />
        <Route path="/tools" element={<ToolsPage />} />
        <Route path="/agents" element={<AgentsPage />} />
        <Route path="/skills" element={<SkillsPage />} />
        <Route path="/system" element={<SystemPage />} />
      </Routes>
    </PageTransition>
  )
}

export default function App() {
  return (
    <HashRouter>
      <div className="h-screen w-screen overflow-hidden bg-bg text-text relative">
        <main className="h-full w-full overflow-hidden">
          <AppRoutes />
        </main>
        <ThemeToggle />
        <SideDock />
      </div>
    </HashRouter>
  )
}
