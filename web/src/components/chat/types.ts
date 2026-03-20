export interface ContentBlock {
  type: string
  text?: string
  id?: string
  name?: string
  input?: object
  tool_use_id?: string
  content?: string
  is_error?: boolean
}

export interface ChatMessage {
  id: string
  role: 'user' | 'assistant'
  content: ContentBlock[]
}
