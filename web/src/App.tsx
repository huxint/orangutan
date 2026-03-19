import { HashRouter, Routes, Route, Navigate } from 'react-router-dom'
import { Sidebar } from './components/layout/Sidebar'
import { TopBar } from './components/layout/TopBar'
import { ChatView } from './components/chat/ChatView'
import { ConfigPage } from './components/admin/ConfigPage'
import { ToolsPage } from './components/admin/ToolsPage'
import { AgentsPage } from './components/admin/AgentsPage'
import { SkillsPage } from './components/admin/SkillsPage'
import { SystemPage } from './components/admin/SystemPage'

export default function App() {
  return (
    <HashRouter>
      <div className="h-screen flex flex-col bg-bg text-text">
        <TopBar />
        <div className="flex flex-1 overflow-hidden">
          <Sidebar />
          <main className="flex-1 overflow-hidden">
            <Routes>
              <Route path="/" element={<Navigate to="/chat" replace />} />
              <Route path="/chat" element={<ChatView />} />
              <Route path="/chat/:sessionId" element={<ChatView />} />
              <Route path="/config" element={<ConfigPage />} />
              <Route path="/tools" element={<ToolsPage />} />
              <Route path="/agents" element={<AgentsPage />} />
              <Route path="/skills" element={<SkillsPage />} />
              <Route path="/system" element={<SystemPage />} />
            </Routes>
          </main>
        </div>
      </div>
    </HashRouter>
  )
}
