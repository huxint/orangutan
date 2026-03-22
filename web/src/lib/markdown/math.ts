function normalizeDelimitedBlockMath(text: string): string {
  return text.replace(/\\\[((?:.|\n)+?)\\\]/g, (_match, content: string) => {
    const trimmed = content.trim();
    return `\n\n$$\n${trimmed}\n$$\n\n`;
  });
}

function normalizeDelimitedInlineMath(text: string): string {
  return text.replace(/\\\(([\s\S]+?)\\\)/g, (_match, content: string) => {
    const trimmed = content.trim();
    return trimmed ? `$${trimmed}$` : "";
  });
}

function normalizeDollarBlockMath(text: string): string {
  return text.replace(/\$\$([\s\S]+?)\$\$/g, (_match, content: string) => {
    const trimmed = content.trim();
    return `\n\n$$\n${trimmed}\n$$\n\n`;
  });
}

export function normalizeMarkdownMath(text: string): string {
  let result = text;
  result = normalizeDelimitedBlockMath(result);
  result = normalizeDelimitedInlineMath(result);
  result = normalizeDollarBlockMath(result);
  result = result.replace(/\n{3,}/g, "\n\n");
  return result;
}
