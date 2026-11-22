import ReactMarkdown from 'react-markdown';
import remarkGfm from 'remark-gfm';

export default function RichText({ children }: { children: string }) {
  return (
    <div data-rich-text>
      <ReactMarkdown
        remarkPlugins={[remarkGfm]}
        skipHtml
        components={{
          a: ({ children, href }) => (
            <a href={href} target="_blank" rel="noreferrer noopener">
              {children}
            </a>
          ),
        }}
      >
        {children}
      </ReactMarkdown>
    </div>
  );
}
