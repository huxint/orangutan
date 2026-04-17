/// Tokenise inline markdown into RichInline items suitable for pretext layout.
/// Recognises:
///   - backtick code spans  → monospace chip token
///   - [text](href)          → link token
///   - @handle               → mention chip
///   - plain text            → single text run
///
/// Each token keeps `break`/`extraWidth` hints so pretext can decide line breaks
/// without guessing, and so the DOM layer renders matching widths.

export type InlineTokenKind = "text" | "code" | "link" | "mention" | "chip";

export interface InlineToken {
  kind: InlineTokenKind;
  text: string;
  href?: string;
  /// Extra pixel width beyond the glyph advance (chip padding, border).
  extraWidth?: number;
  /// Passed through to pretext to prevent mid-token breaks.
  break?: "normal" | "never";
}

const CODE_SPAN = /`([^`\n]+)`/g;
const LINK = /\[([^\]]+)\]\(([^)\s]+)\)/g;
const MENTION = /(^|\s)(@[a-zA-Z0-9_][a-zA-Z0-9_-]{1,40})/g;

interface RawSpan {
  start: number;
  end: number;
  kind: InlineTokenKind;
  text: string;
  href?: string;
}

function* collect(regex: RegExp, text: string, kind: InlineTokenKind, capture = 1, hrefCapture?: number): Generator<RawSpan> {
  regex.lastIndex = 0;
  let m: RegExpExecArray | null;
  while ((m = regex.exec(text)) !== null) {
    const captureText = m[capture] ?? "";
    const start = m.index + (m[0].indexOf(captureText));
    yield {
      start,
      end: start + captureText.length,
      kind,
      text: captureText,
      href: hrefCapture !== undefined ? m[hrefCapture] : undefined,
    };
  }
}

export function tokenizeInline(source: string): InlineToken[] {
  if (!source) return [];
  const spans: RawSpan[] = [];
  for (const s of collect(CODE_SPAN, source, "code")) spans.push(s);
  for (const s of collect(LINK, source, "link", 1, 2)) spans.push(s);
  for (const s of collect(MENTION, source, "mention", 2)) spans.push(s);
  spans.sort((a, b) => a.start - b.start);

  const nonOverlapping: RawSpan[] = [];
  let cursor = 0;
  for (const span of spans) {
    if (span.start < cursor) continue;
    nonOverlapping.push(span);
    cursor = span.end;
  }

  const tokens: InlineToken[] = [];
  let offset = 0;
  for (const span of nonOverlapping) {
    if (span.start > offset) {
      tokens.push({ kind: "text", text: source.slice(offset, span.start) });
    }
    const extraWidth = span.kind === "code" || span.kind === "mention" || span.kind === "chip" ? 16 : 0;
    tokens.push({
      kind: span.kind,
      text: span.text,
      href: span.href,
      extraWidth,
      break: span.kind === "code" || span.kind === "mention" ? "never" : undefined,
    });
    offset = span.end;
  }
  if (offset < source.length) tokens.push({ kind: "text", text: source.slice(offset) });
  return tokens;
}

/// Font descriptors for pretext (it consumes the same string as `ctx.font`).
export interface InlineFontSet {
  body: string;
  mono: string;
}

export function defaultFonts(size = 16): InlineFontSet {
  return {
    body: `${size}px "Inter Tight", ui-sans-serif, system-ui, sans-serif`,
    mono: `${Math.round(size * 0.9)}px "JetBrains Mono", ui-monospace, monospace`,
  };
}

export function tokenFont(token: InlineToken, set: InlineFontSet): string {
  return token.kind === "code" ? set.mono : set.body;
}
