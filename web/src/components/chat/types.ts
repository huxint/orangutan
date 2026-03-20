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

export interface SessionMessage {
  role: string
  content: ContentBlock[]
}

export interface ChatSessionSummary {
  id: string
  created_at: string
  model: string
  scope_key: string
  agent_key: string
  origin_kind: string
  origin_ref: string
  message_count: number
  read_only: boolean
  preview?: string
}

export interface ChatSessionData extends ChatSessionSummary {
  messages: SessionMessage[]
}

export interface ChatMessage {
  id: string
  role: 'user' | 'assistant'
  content: ContentBlock[]
}
