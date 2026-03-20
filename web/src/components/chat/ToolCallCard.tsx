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
    <div className="my-3 overflow-hidden rounded-xl border border-border bg-bg/70 shadow-sm">
      <button
        onClick={() => setOpen(!open)}
        className="flex w-full items-center gap-2 px-3 py-2.5 text-left text-sm text-text-muted transition-colors hover:text-text"
      >
        {open ? <ChevronDown size={14} /> : <ChevronRight size={14} />}
        <Wrench size={14} className="text-accent" />
        <span className="font-medium text-text">{name}</span>
        {running && <span className="ml-auto text-xs text-accent animate-pulse">running...</span>}
        {result?.is_error && <span className="ml-auto text-xs text-red-400">error</span>}
        {result && !result.is_error && <span className="ml-auto text-xs text-text-muted">done</span>}
      </button>
      {open && (
        <div className="space-y-2 border-t border-border px-3 py-3">
          <div className="text-[11px] font-medium tracking-wide text-text-muted uppercase">Input</div>
          <pre className="overflow-x-auto rounded-lg bg-bg px-3 py-2 text-xs text-text-muted">
            {JSON.stringify(input, null, 2)}
          </pre>
          {result && (
            <>
              <div className="text-[11px] font-medium tracking-wide text-text-muted uppercase">Result</div>
              <pre className={`overflow-x-auto rounded-lg bg-bg px-3 py-2 text-xs ${result.is_error ? 'text-red-400' : 'text-text-muted'}`}>
                {result.content}
              </pre>
            </>
          )}
          {running && (
            <div className="text-xs text-text-muted">
              Waiting for tool result...
            </div>
          )}
        </div>
      )}
    </div>
  )
}
