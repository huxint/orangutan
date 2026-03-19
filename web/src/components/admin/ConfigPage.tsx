import { useEffect, useState } from 'react'
import { apiFetch } from '../../api/client'

interface Memory {
  mirror_enabled: boolean
  mirror_file: string
  journal_dir: string
}

interface Config {
  provider: string
  model: string
  base_url: string
  temperature: number
  max_iterations: number
  max_tokens: number
  workspace: string
  edit_mode: string
  system_prompt: string
  auto_save: boolean
  allowed_tools: string[]
  denied_tools: string[]
  fallback_models: string[]
  memory: Memory
}

const inputClass = 'w-full bg-bg-surface border border-border rounded px-3 py-2 text-text focus:outline-none focus:border-accent'

function Section({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <div className="space-y-3">
      <h2 className="text-lg font-medium text-accent">{title}</h2>
      <div className="space-y-3">{children}</div>
    </div>
  )
}

function Field({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <label className="block space-y-1">
      <span className="text-sm text-text-muted">{label}</span>
      {children}
    </label>
  )
}

export function ConfigPage() {
  const [config, setConfig] = useState<Config | null>(null)
  const [error, setError] = useState('')
  const [saving, setSaving] = useState(false)
  const [message, setMessage] = useState('')

  const load = () => {
    setError('')
    apiFetch<Config>('/api/config').then(setConfig).catch(e => setError(e.message))
  }

  useEffect(load, [])

  if (error) return (
    <div className="p-6">
      <h1 className="text-2xl font-semibold mb-4">Configuration</h1>
      <p className="text-red-400 mb-2">{error}</p>
      <button onClick={load} className="px-3 py-1 bg-accent text-white rounded hover:bg-accent-hover">Retry</button>
    </div>
  )
  if (!config) return <div className="p-6"><h1 className="text-2xl font-semibold mb-4">Configuration</h1><p className="text-text-muted">Loading...</p></div>

  const set = (key: keyof Config, val: unknown) => setConfig({ ...config, [key]: val })
  const setMem = (key: keyof Memory, val: unknown) => setConfig({ ...config, memory: { ...config.memory, [key]: val } })

  const save = async () => {
    setSaving(true)
    setMessage('')
    try {
      await apiFetch('/api/config', { method: 'PUT', body: JSON.stringify(config) })
      setMessage('Saved')
      setTimeout(() => setMessage(''), 3000)
    } catch (e: unknown) {
      setMessage(`Error: ${e instanceof Error ? e.message : e}`)
    } finally {
      setSaving(false)
    }
  }

  return (
    <div className="p-6 max-w-2xl space-y-6">
      <h1 className="text-2xl font-semibold">Configuration</h1>

      <Section title="Agent">
        <Field label="Provider"><input className={inputClass} value={config.provider} onChange={e => set('provider', e.target.value)} /></Field>
        <Field label="Model"><input className={inputClass} value={config.model} onChange={e => set('model', e.target.value)} /></Field>
        <Field label="Base URL"><input className={inputClass} value={config.base_url} onChange={e => set('base_url', e.target.value)} /></Field>
        <Field label="Temperature"><input className={inputClass} type="number" step="0.1" value={config.temperature} onChange={e => set('temperature', parseFloat(e.target.value) || 0)} /></Field>
        <Field label="Max Iterations"><input className={inputClass} type="number" value={config.max_iterations} onChange={e => set('max_iterations', parseInt(e.target.value) || 0)} /></Field>
        <Field label="Max Tokens"><input className={inputClass} type="number" value={config.max_tokens} onChange={e => set('max_tokens', parseInt(e.target.value) || 0)} /></Field>
        <Field label="Fallback Models (comma-separated)"><input className={inputClass} value={config.fallback_models.join(', ')} onChange={e => set('fallback_models', e.target.value.split(',').map(s => s.trim()).filter(Boolean))} /></Field>
      </Section>

      <Section title="Tools">
        <Field label="Edit Mode"><input className={inputClass} value={config.edit_mode} onChange={e => set('edit_mode', e.target.value)} /></Field>
        <Field label="Allowed Tools (comma-separated)"><input className={inputClass} value={config.allowed_tools.join(', ')} onChange={e => set('allowed_tools', e.target.value.split(',').map(s => s.trim()).filter(Boolean))} /></Field>
        <Field label="Denied Tools (comma-separated)"><input className={inputClass} value={config.denied_tools.join(', ')} onChange={e => set('denied_tools', e.target.value.split(',').map(s => s.trim()).filter(Boolean))} /></Field>
      </Section>

      <Section title="Session">
        <label className="flex items-center gap-2 cursor-pointer">
          <input type="checkbox" checked={config.auto_save} onChange={e => set('auto_save', e.target.checked)} className="accent-accent" />
          <span className="text-sm text-text">Auto Save</span>
        </label>
      </Section>

      <Section title="Memory">
        <label className="flex items-center gap-2 cursor-pointer">
          <input type="checkbox" checked={config.memory.mirror_enabled} onChange={e => setMem('mirror_enabled', e.target.checked)} className="accent-accent" />
          <span className="text-sm text-text">Mirror Enabled</span>
        </label>
        <Field label="Mirror File"><input className={inputClass} value={config.memory.mirror_file} onChange={e => setMem('mirror_file', e.target.value)} /></Field>
        <Field label="Journal Dir"><input className={inputClass} value={config.memory.journal_dir} onChange={e => setMem('journal_dir', e.target.value)} /></Field>
      </Section>

      <div className="flex items-center gap-3">
        <button onClick={save} disabled={saving} className="px-4 py-2 bg-accent text-white rounded hover:bg-accent-hover disabled:opacity-50">
          {saving ? 'Saving...' : 'Save'}
        </button>
        {message && <span className={message.startsWith('Error') ? 'text-red-400' : 'text-green-400'}>{message}</span>}
      </div>
    </div>
  )
}
