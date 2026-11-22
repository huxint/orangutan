import { z } from 'zod';

export const languageSchema = z.enum(['zh', 'en']);
export type Language = z.infer<typeof languageSchema>;
export const statusSchema = z.enum([
  'queued',
  'running',
  'awaiting_approval',
  'cancelling',
  'completed',
  'failed',
  'cancelled',
  'interrupted',
]);
export type Status = z.infer<typeof statusSchema>;
export const connectionSchema = z.object({
  id: z.string(),
  platform: z.enum(['telegram', 'qq', 'feishu']),
  name: z.string(),
  account: z.string(),
  status: z.enum(['disconnected', 'connecting', 'connected', 'error']),
  attempt: z.number().int().nonnegative(),
  simulateFailure: z.boolean(),
});
export const conversationSchema = z.object({
  id: z.string(),
  connectionId: z.string(),
  locator: z.string(),
  requesterId: z.string(),
  sessionId: z.string(),
  name: z.string(),
  customName: z.boolean().optional(),
  origin: z.enum(['desktop', 'channel']),
  unread: z.number().int().nonnegative(),
});
export const messageSchema = z.object({
  id: z.string(),
  conversationId: z.string(),
  sessionId: z.string(),
  role: z.enum(['user', 'assistant']),
  text: z.string(),
  createdAt: z.number(),
  state: z.enum(['complete', 'streaming', 'interrupted']),
});
export const memberSchema = z.object({
  id: z.string(),
  name: z.string(),
  role: z.string(),
  state: z.enum(['waiting', 'working', 'done', 'failed', 'stopped']),
  result: z.string(),
});
export const artifactSchema = z.object({
  name: z.string(),
  kind: z.enum(['markdown', 'text', 'diff']),
  body: z.string(),
});
export const taskSchema = z.object({
  id: z.string(),
  conversationId: z.string(),
  requesterId: z.string(),
  sessionId: z.string(),
  title: z.string(),
  kind: z.enum(['chat', 'file', 'team']),
  status: statusSchema,
  attempt: z.number().int().positive(),
  step: z.number().int().nonnegative(),
  createdAt: z.number(),
  language: languageSchema,
  shouldFail: z.boolean(),
  history: z.array(z.object({ attempt: z.number(), status: statusSchema })),
  members: z.array(memberSchema),
  approval: z
    .object({
      id: z.string(),
      status: z.enum(['pending', 'approved', 'rejected', 'expired', 'cancelled']),
      target: z.string(),
    })
    .nullable(),
  artifact: artifactSchema.nullable(),
});
export const noteSchema = z.object({
  id: z.string(),
  scope: z.enum(['shared', 'personal']),
  ownerId: z.string().nullable(),
  title: z.string(),
  body: z.string(),
  source: z.string(),
  updatedAt: z.number(),
});
export const notificationSchema = z.object({
  id: z.string(),
  eventKey: z.string(),
  conversationId: z.string().nullable(),
  taskId: z.string().nullable(),
  kind: z.enum(['completed', 'failed', 'approval', 'connection']),
  read: z.boolean(),
  title: z.string(),
  detail: z.string(),
});
export const stateSchema = z
  .object({
    version: z.literal(1),
    settings: z.object({
      language: languageSchema,
      theme: z.enum(['system', 'light', 'dark']),
      notifications: z.boolean(),
      memory: z.boolean(),
      model: z.enum(['Claude Sonnet', 'GPT-5']),
      permission: z.enum(['ask', 'allow', 'deny']),
    }),
    selectedId: z.string(),
    connections: z.array(connectionSchema),
    conversations: z.array(conversationSchema),
    messages: z.array(messageSchema),
    tasks: z.array(taskSchema),
    notes: z.array(noteSchema),
    notifications: z.array(notificationSchema),
    drafts: z.record(z.string(), z.string()),
  })
  .superRefine((state, ctx) => {
    const invalid = (message: string) => ctx.addIssue({ code: 'custom', message });
    const conversations = new Map(state.conversations.map((c) => [c.id, c]));
    const connections = new Set(state.connections.map((c) => c.id));
    for (const records of [
      state.connections,
      state.conversations,
      state.messages,
      state.tasks,
      state.notes,
      state.notifications,
    ]) {
      if (new Set(records.map((r) => r.id)).size !== records.length)
        invalid('Duplicate identities');
    }
    if (!conversations.has(state.selectedId)) invalid('Missing selected conversation');
    for (const c of state.conversations) {
      if (c.id !== conversationKey(c.connectionId, c.locator))
        invalid('Invalid composite identity');
      if (c.origin === 'channel' && !connections.has(c.connectionId)) invalid('Missing channel');
      if (c.origin === 'desktop' && c.connectionId !== 'desktop') invalid('Invalid local identity');
    }
    for (const m of state.messages)
      if (!conversations.has(m.conversationId)) invalid('Orphan message');
    for (const t of state.tasks) {
      const c = conversations.get(t.conversationId);
      if (!c || c.requesterId !== t.requesterId || c.sessionId !== t.sessionId)
        invalid('Invalid task ownership');
    }
    for (const n of state.notes)
      if ((n.scope === 'shared') !== (n.ownerId === null)) invalid('Invalid memory scope');
  });

export const conversationKey = (connectionId: string, locator: string) =>
  JSON.stringify([connectionId, locator]);
export const requesterKey = (connectionId: string, senderId: string) =>
  JSON.stringify([connectionId, senderId]);
export type AppState = z.infer<typeof stateSchema>;
export type Conversation = z.infer<typeof conversationSchema>;
export type Task = z.infer<typeof taskSchema>;
export type Note = z.infer<typeof noteSchema>;
export type Connection = z.infer<typeof connectionSchema>;
export type Notification = z.infer<typeof notificationSchema>;
export type Artifact = z.infer<typeof artifactSchema>;
