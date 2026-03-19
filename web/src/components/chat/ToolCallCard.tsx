import { useState } from 'react'
import { Wrench, ChevronRight, ChevronDown } from 'lucide-react'

interface ToolCallCardProps {
  id: string
  name: string
  input: object
  result?: { content: string; is_error: boolean }
}

export function ToolCallCard({ name, input, result }: ToolCallCardProps) {
  const running = !result
  const [open, setOpen] = useState(running)

  return (
    <div className="border-l-2 border-accent rounded-r-lg bg-bg-surface my-2">
      <button
        onClick={() => setOpen(!open)}
        className="flex items-center gap-2 w-full px-3 py-2 text-sm text-text-muted hover:text-text transition-colors"
      >
        {open ? <ChevronDown size={14} /> : <ChevronRight size={14} />}
        <Wrench size={14} className="text-accent" />
        <span className="font-medium">{name}</span>
        {running && <span className="ml-auto text-xs text-accent animate-pulse">running...</span>}
        {result?.is_error && <span className="ml-auto text-xs text-red-400">error</span>}
      </button>
      {open && (
        <div className="px-3 pb-3 space-y-2">
          <pre className="text-xs rounded bg-bg p-2 overflow-x-auto text-text-muted">
            {JSON.stringify(input, null, 2)}
          </pre>
          {result && (
            <pre className={`text-xs rounded bg-bg p-2 overflow-x-auto ${result.is_error ? 'text-red-400' : 'text-text-muted'}`}>
              {result.content}
            </pre>
          )}
        </div>
      )}
    </div>
  )
}
