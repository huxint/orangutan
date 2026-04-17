import { useMemo } from "react";
import {
  layoutNextRichInlineLineRange,
  materializeRichInlineLineRange,
  prepareRichInline,
  type RichInlineCursor,
  type RichInlineFragment,
  type RichInlineItem,
  type RichInlineLine,
} from "@chenglou/pretext/rich-inline";
import {
  defaultFonts,
  tokenFont,
  tokenizeInline,
  type InlineToken,
} from "./inlineTokens";
import { cn } from "../lib/utils";

interface StreamTextProps {
  /// Full accumulated assistant text. The renderer treats it as append-only
  /// and re-measures only new content each frame.
  text: string;
  maxWidth: number;
  fontSize?: number;
  className?: string;
  streaming?: boolean;
  onTokenClick?: (token: InlineToken) => void;
}

interface LineRender {
  fragments: Array<{ fragment: RichInlineFragment; token: InlineToken }>;
  width: number;
}

/// Pretext-backed streaming renderer.
///   - Token stream re-parses only the tail when new characters arrive.
///   - Per-line layout uses the RichInline walker; completed lines are cached.
///   - Each line is absolutely positioned by line-height, so appending new
///     text to the final line does NOT reflow previously laid-out lines.
export function StreamText({
  text,
  maxWidth,
  fontSize = 16,
  className,
  streaming = false,
  onTokenClick,
}: StreamTextProps) {
  const fonts = useMemo(() => defaultFonts(fontSize), [fontSize]);
  const lineHeight = Math.round(fontSize * 1.55);
  const tokens = useMemo(() => tokenizeInline(text), [text]);
  const width = Math.max(120, Math.floor(maxWidth));

  const lines = useMemo<LineRender[]>(() => {
    if (tokens.length === 0) return [];
    const items: RichInlineItem[] = tokens.map((t) => ({
      text: t.text,
      font: tokenFont(t, fonts),
      extraWidth: t.extraWidth,
      break: t.break,
    }));
    const prepared = prepareRichInline(items);
    const rendered: LineRender[] = [];
    let cursor: RichInlineCursor | undefined;
    for (let i = 0; i < 4_000; i++) {
      const range = layoutNextRichInlineLineRange(prepared, width, cursor);
      if (!range) break;
      const line: RichInlineLine = materializeRichInlineLineRange(prepared, range);
      rendered.push({
        width: line.width,
        fragments: line.fragments.map((fragment) => ({
          fragment,
          token: tokens[fragment.itemIndex],
        })),
      });
      cursor = range.end;
    }
    return rendered;
  }, [tokens, fonts, width]);

  return (
    <div
      className={cn("relative text-[var(--color-text)] leading-tight", className)}
      style={{
        width,
        height: Math.max(lineHeight, lines.length * lineHeight),
        fontFamily: "Inter Tight, ui-sans-serif, system-ui, sans-serif",
        fontSize,
      }}
    >
      {lines.map((line, li) => (
        <div
          key={li}
          className="absolute inset-x-0"
          style={{
            top: li * lineHeight,
            height: lineHeight,
            whiteSpace: "nowrap",
          }}
        >
          {line.fragments.map(({ fragment, token }, fi) => (
            <Fragment
              key={fi}
              fragment={fragment}
              token={token}
              fontSize={fontSize}
              onClick={onTokenClick}
            />
          ))}
        </div>
      ))}
      {streaming && (
        <span
          aria-hidden
          className="inline-block align-baseline"
          style={{
            position: "absolute",
            left: (lines[lines.length - 1]?.width ?? 0) + 2,
            top: Math.max(0, (lines.length > 0 ? lines.length - 1 : 0) * lineHeight + 2),
            width: 2,
            height: fontSize,
            background: "var(--color-accent)",
            animation: "caret 1s steps(1) infinite",
          }}
        />
      )}
    </div>
  );
}

interface FragmentProps {
  fragment: RichInlineFragment;
  token: InlineToken;
  fontSize: number;
  onClick?: (token: InlineToken) => void;
}

function Fragment({ fragment, token, fontSize, onClick }: FragmentProps) {
  const left = fragment.gapBefore;
  const width = fragment.occupiedWidth;
  const common: React.CSSProperties = {
    position: "absolute",
    left,
    top: 0,
    height: fontSize * 1.55,
    lineHeight: `${fontSize * 1.55}px`,
    whiteSpace: "pre",
  };
  switch (token.kind) {
    case "code":
      return (
        <code
          className="inline"
          onClick={() => onClick?.(token)}
          style={{
            ...common,
            width,
            fontFamily: 'JetBrains Mono, ui-monospace, monospace',
            fontSize: fontSize * 0.9,
            padding: "0 6px",
            display: "inline-flex",
            alignItems: "center",
            background: "var(--color-bg-2)",
            borderRadius: 6,
            border: "1px solid var(--color-line)",
            color: "var(--color-text)",
          }}
        >
          {fragment.text}
        </code>
      );
    case "link":
      return (
        <a
          href={token.href ?? "#"}
          target="_blank"
          rel="noopener noreferrer"
          style={{
            ...common,
            color: "var(--color-accent)",
            textDecoration: "underline",
            textUnderlineOffset: 3,
          }}
        >
          {fragment.text}
        </a>
      );
    case "mention":
      return (
        <span
          onClick={() => onClick?.(token)}
          style={{
            ...common,
            width,
            display: "inline-flex",
            alignItems: "center",
            padding: "0 6px",
            background: "var(--color-accent-soft)",
            color: "var(--color-accent)",
            borderRadius: 999,
            fontSize: fontSize * 0.92,
            cursor: "pointer",
          }}
        >
          {fragment.text}
        </span>
      );
    case "chip":
      return (
        <span
          onClick={() => onClick?.(token)}
          style={{
            ...common,
            width,
            display: "inline-flex",
            alignItems: "center",
            padding: "0 8px",
            background: "var(--color-aux-soft)",
            color: "var(--color-aux)",
            borderRadius: 999,
            fontSize: fontSize * 0.88,
            cursor: "pointer",
          }}
        >
          {fragment.text}
        </span>
      );
    default:
      return <span style={common}>{fragment.text}</span>;
  }
}
