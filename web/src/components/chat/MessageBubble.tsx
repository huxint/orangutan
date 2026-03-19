import ReactMarkdown from 'react-markdown'
import remarkGfm from 'remark-gfm'
import rehypeHighlight from 'rehype-highlight'

interface MessageBubbleProps {
  role: 'user' | 'assistant'
  text: string
}

export function MessageBubble({ role, text }: MessageBubbleProps) {
  if (role === 'user') {
    return (
      <div className="flex justify-end">
        <div className="max-w-[75%] rounded-lg bg-accent-bg px-4 py-2 text-text whitespace-pre-wrap">
          {text}
        </div>
      </div>
    )
  }

  return (
    <div className="max-w-[75%]">
      <div className="prose prose-invert prose-sm max-w-none text-text">
        <ReactMarkdown
          remarkPlugins={[remarkGfm]}
          rehypePlugins={[rehypeHighlight]}
          components={{
            pre({ children }) {
              return (
                <pre className="rounded-lg bg-bg-surface p-3 overflow-x-auto">
                  {children}
                </pre>
              )
            },
            code({ className, children, ...props }) {
              const isInline = !className
              if (isInline) {
                return (
                  <code className="rounded bg-bg-surface border border-border px-1 py-0.5 text-sm" {...props}>
                    {children}
                  </code>
                )
              }
              return <code className={className} {...props}>{children}</code>
            },
          }}
        >
          {text}
        </ReactMarkdown>
      </div>
    </div>
  )
}
