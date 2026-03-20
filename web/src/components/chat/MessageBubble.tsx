import ReactMarkdown from 'react-markdown'
import remarkGfm from 'remark-gfm'
import rehypeHighlight from 'rehype-highlight'
import { ToolCallCard } from './ToolCallCard'
import type { ContentBlock } from './types'

interface MessageBubbleProps {
  role: 'user' | 'assistant'
  content: ContentBlock[]
}

function AssistantMarkdown({ text }: { text: string }) {
  return (
    <ReactMarkdown
      remarkPlugins={[remarkGfm]}
      rehypePlugins={[rehypeHighlight]}
      components={{
        p({ children }) {
          return <p className="my-3 leading-7 first:mt-0 last:mb-0">{children}</p>
        },
        h1({ children }) {
          return <h1 className="mt-5 mb-3 text-xl font-semibold tracking-tight first:mt-0">{children}</h1>
        },
        h2({ children }) {
          return <h2 className="mt-5 mb-3 text-lg font-semibold tracking-tight first:mt-0">{children}</h2>
        },
        h3({ children }) {
          return <h3 className="mt-4 mb-2 text-base font-semibold first:mt-0">{children}</h3>
        },
        ul({ children }) {
          return <ul className="my-3 list-disc space-y-1.5 pl-6 marker:text-text-muted">{children}</ul>
        },
        ol({ children }) {
          return <ol className="my-3 list-decimal space-y-1.5 pl-6 marker:text-text-muted">{children}</ol>
        },
        li({ children }) {
          return <li className="leading-7">{children}</li>
        },
        blockquote({ children }) {
          return (
            <blockquote className="my-4 rounded-r-xl border-l-3 border-accent/70 bg-accent-bg px-4 py-3 text-[0.96rem] text-text/90">
              {children}
            </blockquote>
          )
        },
        a({ href, children }) {
          return (
            <a href={href} className="font-medium text-accent underline decoration-accent/40 underline-offset-3 hover:decoration-accent">
              {children}
            </a>
          )
        },
        hr() {
          return <hr className="my-5 border-border" />
        },
        table({ children }) {
          return (
            <div className="my-4 overflow-x-auto rounded-xl border border-border">
              <table className="min-w-full border-collapse text-sm">{children}</table>
            </div>
          )
        },
        thead({ children }) {
          return <thead className="bg-bg/70">{children}</thead>
        },
        th({ children }) {
          return <th className="border-b border-border px-3 py-2 text-left font-medium">{children}</th>
        },
        td({ children }) {
          return <td className="border-t border-border px-3 py-2 align-top">{children}</td>
        },
        pre({ children }) {
          return (
            <pre className="my-4 overflow-x-auto rounded-xl border border-border bg-bg px-4 py-3 text-[13px] leading-6 shadow-sm">
              {children}
            </pre>
          )
        },
        code({ className, children, ...props }) {
          const isInline = !className
          if (isInline) {
            return (
              <code
                className="rounded-md border border-border bg-bg px-1.5 py-0.5 font-mono text-[0.92em] text-accent"
                {...props}
              >
                {children}
              </code>
            )
          }

          return <code className={className} {...props}>{children}</code>
        },
        strong({ children }) {
          return <strong className="font-semibold text-text">{children}</strong>
        },
      }}
    >
      {text}
    </ReactMarkdown>
  )
}

function renderAssistantBlocks(content: ContentBlock[]) {
  const blocks = []
  let textBuffer = ''

  function flushTextBuffer() {
    if (!textBuffer) return
    blocks.push(
      <div key={`text-${blocks.length}`} className="text-[15px] text-text">
        <AssistantMarkdown text={textBuffer} />
      </div>,
    )
    textBuffer = ''
  }

  for (let index = 0; index < content.length; index += 1) {
    const block = content[index]

    if (block.type === 'text' && block.text) {
      textBuffer += block.text
      continue
    }

    flushTextBuffer()

    if (block.type === 'tool_use' && block.id) {
      const next = content[index + 1]
      const result = next?.type === 'tool_result' && next.tool_use_id === block.id
        ? { content: next.content ?? '', is_error: next.is_error ?? false }
        : undefined

      blocks.push(
        <ToolCallCard
          key={block.id}
          id={block.id}
          name={block.name ?? 'tool'}
          input={block.input ?? {}}
          result={result}
        />,
      )

      if (result) {
        index += 1
      }
      continue
    }

    if (block.type === 'tool_result' && block.tool_use_id) {
      blocks.push(
        <ToolCallCard
          key={`result-${block.tool_use_id}-${blocks.length}`}
          id={block.tool_use_id}
          name="tool"
          input={{}}
          result={{ content: block.content ?? '', is_error: block.is_error ?? false }}
        />,
      )
    }
  }

  flushTextBuffer()
  return blocks
}

export function MessageBubble({ role, content }: MessageBubbleProps) {
  if (role === 'user') {
    const text = content.filter(block => block.type === 'text' && block.text).map(block => block.text).join('')

    return (
      <div className="flex justify-end">
        <div className="max-w-[78%] rounded-2xl bg-accent-bg px-4 py-3 text-text whitespace-pre-wrap shadow-sm ring-1 ring-accent/10">
          {text}
        </div>
      </div>
    )
  }

  return (
    <div className="max-w-[82%]">
      <div className="space-y-3 rounded-2xl border border-border/80 bg-bg-surface/70 px-4 py-3 shadow-sm">
        {renderAssistantBlocks(content)}
      </div>
    </div>
  )
}
