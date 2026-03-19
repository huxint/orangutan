import { useEffect, useState, useRef } from 'react'
import { apiFetch } from '../../api/client'

interface SystemStatus {
  uptime_seconds: number
  active_web_sessions: number
  provider_health: Record<string, unknown>
  cron: Record<string, unknown>
  heartbeat: Record<string, unknown>
}

function formatUptime(s: number): string {
  const h = Math.floor(s / 3600)
  const m = Math.floor((s % 3600) / 60)
  const sec = Math.floor(s % 60)
  return `${h}h ${m}m ${sec}s`
}

export function SystemPage() {
  const [status, setStatus] = useState<SystemStatus | null>(null)
  const [error, setError] = useState('')
  const intervalRef = useRef<ReturnType<typeof setInterval>>(undefined)

  const load = () => {
    setError('')
    apiFetch<SystemStatus>('/api/system').then(setStatus).catch(e => setError(e.message))
  }

  useEffect(() => {
    load()
    intervalRef.current = setInterval(load, 30000)
    return () => clearInterval(intervalRef.current)
  }, [])

  return (
    <div className="p-6">
      <h1 className="text-2xl font-semibold mb-4">System</h1>
      {error ? (
        <div>
          <p className="text-red-400 mb-2">{error}</p>
          <button onClick={load} className="px-3 py-1 bg-accent text-white rounded hover:bg-accent-hover">Retry</button>
        </div>
      ) : !status ? (
        <p className="text-text-muted">Loading...</p>
      ) : (
        <div className="grid grid-cols-1 sm:grid-cols-3 gap-4">
          <div className="bg-bg-surface border border-border rounded-lg p-4">
            <p className="text-sm text-text-muted mb-1">Uptime</p>
            <p className="text-xl font-medium text-text">{formatUptime(status.uptime_seconds)}</p>
          </div>
          <div className="bg-bg-surface border border-border rounded-lg p-4">
            <p className="text-sm text-text-muted mb-1">Web Sessions</p>
            <p className="text-xl font-medium text-text">{status.active_web_sessions}</p>
          </div>
          <div className="bg-bg-surface border border-border rounded-lg p-4">
            <p className="text-sm text-text-muted mb-1">Status</p>
            <div className="flex items-center gap-2">
              <span className="inline-block w-2.5 h-2.5 rounded-full bg-green-500" />
              <span className="text-xl font-medium text-text">Healthy</span>
            </div>
          </div>
        </div>
      )}
    </div>
  )
}
