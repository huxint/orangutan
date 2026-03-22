import { Children, isValidElement, type ReactNode } from 'react'
import ReactMarkdown from 'react-markdown'
import remarkGfm from 'remark-gfm'
import remarkMath from 'remark-math'
import rehypeHighlight from 'rehype-highlight'
import rehypeKatex from 'rehype-katex'
import { ToolCallCard } from './ToolCallCard'
import { normalizeMarkdownMath } from '../../lib/markdown/math'
import type { ContentBlock } from './types'

interface MessageBubbleProps {
  role: 'user' | 'assistant'
  content: ContentBlock[]
}

/**
 * Detect if children contain a block-level element (like katex-display div).
 * If so, we must render a <div> instead of <p> to avoid invalid HTML nesting
 * which causes the browser to break the layout.
 */
function hasBlockChild(children: ReactNode): boolean {
  return Children.toArray(children).some(child => {
    if (!isValidElement(child)) return false
    const props = child.props as Record<string, unknown>
    // rehype-katex wraps display math in <span class="katex-display">
    // but the real issue is any div/span that acts as block
    if (typeof props.className === 'string' && props.className.includes('katex-display')) {
      return true
    }
    // Also check for div type
    if (child.type === 'div') return true
    return false
  })
}

function AssistantMarkdown({ text }: { text: string }) {
  const processed = normalizeMarkdownMath(text)
  return (
    <ReactMarkdown
      remarkPlugins={[remarkGfm, remarkMath]}
      rehypePlugins={[[rehypeHighlight], [rehypeKatex, { strict: false, trust: true, minRuleThickness: 0.06, output: 'htmlAndMathml' }]]}
      components={{
        p({ children, node, ...rest }) {
          // If this paragraph contains display math, use div to avoid
          // <p> containing block elements (invalid HTML → broken layout)
          if (hasBlockChild(children)) {
            return <div className="my-2.5 leading-[1.75] first:mt-0 last:mb-0" {...rest}>{children}</div>
          }
          return <p className="my-2.5 leading-[1.75] first:mt-0 last:mb-0" {...rest}>{children}</p>
        },
        h1({ children }) {
          return <h1 className="mt-6 mb-3 text-lg font-bold first:mt-0">{children}</h1>
        },
        h2({ children }) {
          return <h2 className="mt-5 mb-2.5 text-base font-bold first:mt-0">{children}</h2>
        },
        h3({ children }) {
          return <h3 className="mt-4 mb-2 text-sm font-bold first:mt-0">{children}</h3>
        },
        ul({ children }) {
          return <ul className="my-2.5 list-disc space-y-1 pl-5 marker:text-text-muted">{children}</ul>
        },
        ol({ children }) {
          return <ol className="my-2.5 list-decimal space-y-1 pl-5 marker:text-text-muted">{children}</ol>
        },
        li({ children }) {
          return <li className="leading-[1.75]">{children}</li>
        },
        blockquote({ children }) {
          return (
            <blockquote className="my-3 border-l-2 border-accent/40 pl-4 text-text-secondary italic">
              {children}
            </blockquote>
          )
        },
        a({ href, children }) {
          return (
            <a
              href={href}
              className="text-accent underline decoration-accent/30 underline-offset-2 hover:decoration-accent transition-colors"
              target="_blank"
              rel="noopener noreferrer"
            >
              {children}
            </a>
          )
        },
        hr() {
          return <hr className="my-5 border-border" />
        },
        table({ children }) {
          return (
            <div className="my-3 overflow-x-auto rounded-lg border border-border">
              <table className="min-w-full text-sm">{children}</table>
            </div>
          )
        },
        thead({ children }) {
          return <thead className="bg-bg-surface">{children}</thead>
        },
        th({ children }) {
          return <th className="border-b border-border px-3 py-2 text-left text-xs font-semibold uppercase tracking-wider text-text-secondary">{children}</th>
        },
        td({ children }) {
          return <td className="border-t border-border-subtle px-3 py-2">{children}</td>
        },
        pre({ children }) {
          return (
            <pre className="my-3 overflow-x-auto rounded-lg bg-bg-elevated border border-border px-4 py-3 text-[13px] leading-relaxed">
              {children}
            </pre>
          )
        },
        code({ className, children, ...props }) {
          if (!className) {
            return (
              <code
                className="rounded bg-accent-dim px-1.5 py-0.5 font-mono text-[0.88em] text-accent"
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
      {processed}
    </ReactMarkdown>
  )
}

function renderAssistantBlocks(content: ContentBlock[]) {
  const blocks = []
  let textBuffer = ''

  function flushTextBuffer() {
    if (!textBuffer) return
    blocks.push(
      <div key={`text-${blocks.length}`} className="text-[14.5px] text-text/90">
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

      if (result) index += 1
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
    const text = content.filter(b => b.type === 'text' && b.text).map(b => b.text).join('')
    return (
      <div className="flex justify-end">
        <div className="max-w-[75%] rounded-2xl rounded-br-sm px-4 py-2.5 text-[14.5px]
          bg-accent text-white leading-relaxed whitespace-pre-wrap
          shadow-[0_2px_12px_rgba(249,115,22,0.15)]">
          {text}
        </div>
      </div>
    )
  }

  return (
    <div className="max-w-[85%]">
      <div className="space-y-2 rounded-2xl rounded-bl-sm
        bg-bg-surface border border-border
        px-4 py-3">
        {renderAssistantBlocks(content)}
      </div>
    </div>
  )
}
