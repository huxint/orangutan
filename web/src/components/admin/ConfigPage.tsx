import { useEffect, useState } from 'react'
import {
  Save, RotateCcw, CheckCircle, AlertCircle,
  Bot, Wrench, HardDrive, BrainCircuit,
  ChevronDown,
  type LucideIcon,
} from 'lucide-react'
import { motion, AnimatePresence } from 'framer-motion'
import { apiFetch } from '../../api/client'
import { cn } from '../../lib/utils'

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

const inputCls = cn(
  'w-full rounded-lg border border-border bg-bg px-3 py-2 text-sm text-text',
  'placeholder:text-text-muted focus:outline-none focus:ring-1 focus:ring-accent/30 focus:border-accent/40',
  'transition-all duration-150',
)

function Field({ label, hint, children }: { label: string; hint?: string; children: React.ReactNode }) {
  return (
    <label className="block space-y-1.5">
      <div className="flex items-baseline justify-between">
        <span className="text-xs font-medium text-text-secondary">{label}</span>
        {hint && <span className="text-[10px] text-text-muted">{hint}</span>}
      </div>
      {children}
    </label>
  )
}

function Toggle({ checked, onChange, label, description }: {
  checked: boolean; onChange: (v: boolean) => void; label: string; description?: string
}) {
  return (
    <div
      className={cn(
        'flex items-center justify-between rounded-lg border px-3.5 py-3 cursor-pointer transition-colors duration-150',
        checked ? 'border-accent/25 bg-accent-dim' : 'border-border bg-bg hover:border-border',
      )}
      onClick={() => onChange(!checked)}
    >
      <div>
        <span className="text-sm font-medium text-text">{label}</span>
        {description && <p className="text-[11px] text-text-muted mt-0.5">{description}</p>}
      </div>
      <div className={cn(
        'relative w-9 h-5 rounded-full transition-colors duration-200 shrink-0 ml-3',
        checked ? 'bg-accent' : 'bg-bg-elevated border border-border',
      )}>
        <div className={cn(
          'absolute top-[3px] left-[3px] w-[14px] h-[14px] rounded-full bg-white shadow-sm transition-transform duration-200',
          checked && 'translate-x-[16px]',
        )} />
      </div>
    </div>
  )
}

function Section({ icon: Icon, title, color, children, defaultOpen = true }: {
  icon: LucideIcon; title: string; color: string; children: React.ReactNode; defaultOpen?: boolean
}) {
  const [open, setOpen] = useState(defaultOpen)

  return (
    <motion.div
      initial={{ opacity: 0, y: 6 }}
      animate={{ opacity: 1, y: 0 }}
      className="rounded-xl border border-border bg-bg-surface overflow-hidden"
    >
      <button
        type="button"
        onClick={() => setOpen(p => !p)}
        className="w-full flex items-center gap-3 px-4 py-3 hover:bg-bg-elevated/50 transition-colors"
      >
        <div className={cn('p-1.5 rounded-lg', color)}>
          <Icon size={14} className="text-current" />
        </div>
        <span className="text-sm font-semibold text-text flex-1 text-left">{title}</span>
        <ChevronDown
          size={14}
          className={cn('text-text-muted transition-transform duration-200', open && 'rotate-180')}
        />
      </button>

      <AnimatePresence initial={false}>
        {open && (
          <motion.div
            initial={{ height: 0 }}
            animate={{ height: 'auto' }}
            exit={{ height: 0 }}
            transition={{ duration: 0.2, ease: [0.25, 0.1, 0.25, 1] }}
            className="overflow-hidden"
          >
            <div className="px-4 pb-4 pt-1 space-y-3 border-t border-border">
              {children}
            </div>
          </motion.div>
        )}
      </AnimatePresence>
    </motion.div>
  )
}

export function ConfigPage() {
  const [config, setConfig] = useState<Config | null>(null)
  const [error, setError] = useState('')
  const [saving, setSaving] = useState(false)
  const [msg, setMsg] = useState('')

  const load = () => {
    setError('')
    apiFetch<Config>('/api/config').then(setConfig).catch(e => setError(e.message))
  }

  useEffect(load, [])

  if (error) return (
    <div className="p-6 space-y-3">
      <h1 className="text-xl font-bold">Configuration</h1>
      <div className="rounded-xl border border-danger/20 bg-danger-dim p-4 text-danger text-sm">
        {error}
        <button onClick={load} className="ml-3 inline-flex items-center gap-1 text-xs underline"><RotateCcw size={12} />Retry</button>
      </div>
    </div>
  )

  if (!config) return (
    <div className="p-6">
      <h1 className="text-xl font-bold mb-3">Configuration</h1>
      <div className="flex items-center gap-2 text-sm text-text-muted">
        <div className="w-3.5 h-3.5 rounded-full border-2 border-accent border-t-transparent animate-spin" />
        Loading...
      </div>
    </div>
  )

  const set = (key: keyof Config, val: unknown) => setConfig({ ...config, [key]: val })
  const setMem = (key: keyof Memory, val: unknown) => setConfig({ ...config, memory: { ...config.memory, [key]: val } })

  const save = async () => {
    setSaving(true); setMsg('')
    try {
      await apiFetch('/api/config', { method: 'PUT', body: JSON.stringify(config) })
      setMsg('ok'); setTimeout(() => setMsg(''), 2500)
    } catch (e: unknown) {
      setMsg(`err:${e instanceof Error ? e.message : e}`)
    } finally { setSaving(false) }
  }

  return (
    <div className="p-6 max-w-2xl space-y-3 overflow-y-auto h-full pb-10">
      {/* Header with sticky save bar */}
      <div className="flex items-center justify-between sticky top-0 z-10 bg-bg/80 backdrop-blur-md py-2 -mt-2 -mx-1 px-1">
        <h1 className="text-xl font-bold">Configuration</h1>
        <div className="flex items-center gap-2.5">
          <AnimatePresence>
            {msg && (
              <motion.span
                initial={{ opacity: 0, x: 8 }}
                animate={{ opacity: 1, x: 0 }}
                exit={{ opacity: 0, x: 8 }}
                className={cn('flex items-center gap-1 text-xs', msg.startsWith('err') ? 'text-danger' : 'text-success')}
              >
                {msg.startsWith('err') ? <AlertCircle size={12} /> : <CheckCircle size={12} />}
                {msg.startsWith('err') ? msg.slice(4) : 'Saved'}
              </motion.span>
            )}
          </AnimatePresence>
          <button onClick={save} disabled={saving}
            className={cn(
              'rounded-lg px-4 py-1.5 text-xs font-semibold text-white flex items-center gap-1.5',
              'transition-all duration-200',
              saving ? 'bg-accent/50' : 'bg-accent hover:bg-accent-hover shadow-[0_2px_8px_rgba(249,115,22,0.15)] hover:shadow-[0_4px_16px_rgba(249,115,22,0.25)]',
            )}>
            <Save size={12} />{saving ? 'Saving...' : 'Save'}
          </button>
        </div>
      </div>

      {/* Agent section */}
      <Section icon={Bot} title="Agent" color="bg-sky-500/10 text-sky-400">
        <div className="grid grid-cols-2 gap-3">
          <Field label="Provider"><input className={inputCls} value={config.provider} onChange={e => set('provider', e.target.value)} /></Field>
          <Field label="Model"><input className={inputCls} value={config.model} onChange={e => set('model', e.target.value)} /></Field>
        </div>
        <Field label="Base URL" hint="API endpoint"><input className={inputCls} value={config.base_url} onChange={e => set('base_url', e.target.value)} /></Field>
        <div className="grid grid-cols-3 gap-2">
          <Field label="Temperature"><input className={inputCls} type="number" step="0.1" value={config.temperature} onChange={e => set('temperature', parseFloat(e.target.value) || 0)} /></Field>
          <Field label="Max Iterations"><input className={inputCls} type="number" value={config.max_iterations} onChange={e => set('max_iterations', parseInt(e.target.value) || 0)} /></Field>
          <Field label="Max Tokens"><input className={inputCls} type="number" value={config.max_tokens} onChange={e => set('max_tokens', parseInt(e.target.value) || 0)} /></Field>
        </div>
        <Field label="Fallback Models" hint="comma-separated"><input className={inputCls} value={config.fallback_models.join(', ')} onChange={e => set('fallback_models', e.target.value.split(',').map(s => s.trim()).filter(Boolean))} /></Field>
      </Section>

      {/* Tools section */}
      <Section icon={Wrench} title="Tools" color="bg-amber-500/10 text-amber-400">
        <Field label="Edit Mode"><input className={inputCls} value={config.edit_mode} onChange={e => set('edit_mode', e.target.value)} /></Field>
        <Field label="Allowed Tools" hint="comma-separated"><input className={inputCls} value={config.allowed_tools.join(', ')} onChange={e => set('allowed_tools', e.target.value.split(',').map(s => s.trim()).filter(Boolean))} /></Field>
        <Field label="Denied Tools" hint="comma-separated"><input className={inputCls} value={config.denied_tools.join(', ')} onChange={e => set('denied_tools', e.target.value.split(',').map(s => s.trim()).filter(Boolean))} /></Field>
      </Section>

      {/* Session section */}
      <Section icon={HardDrive} title="Session" color="bg-emerald-500/10 text-emerald-400" defaultOpen={false}>
        <Toggle checked={config.auto_save} onChange={v => set('auto_save', v)} label="Auto Save" description="Automatically persist sessions to disk" />
      </Section>

      {/* Memory section */}
      <Section icon={BrainCircuit} title="Memory" color="bg-violet-500/10 text-violet-400" defaultOpen={false}>
        <Toggle checked={config.memory.mirror_enabled} onChange={v => setMem('mirror_enabled', v)} label="Mirror Enabled" description="Mirror conversation to a file" />
        <Field label="Mirror File"><input className={inputCls} value={config.memory.mirror_file} onChange={e => setMem('mirror_file', e.target.value)} /></Field>
        <Field label="Journal Dir"><input className={inputCls} value={config.memory.journal_dir} onChange={e => setMem('journal_dir', e.target.value)} /></Field>
      </Section>
    </div>
  )
}
