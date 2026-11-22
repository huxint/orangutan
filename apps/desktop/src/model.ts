import type { AppState, Task, Status, Note, Notification, Language } from './schema.ts';
import { conversationKey, requesterKey } from './schema.ts';

export const active = (status: Status) =>
  ['queued', 'running', 'awaiting_approval', 'cancelling'].includes(status);
export const canControl = (state: AppState, conversationId: string) =>
  state.conversations.some((c) => c.id === conversationId && c.origin === 'desktop');
const say = (language: Language, zh: string, en: string) => (language === 'zh' ? zh : en);

export type Command =
  | { type: 'select'; id: string }
  | { type: 'new'; id: string; now: number }
  | { type: 'rename'; conversationId: string; name: string }
  | { type: 'draft'; conversationId: string; text: string }
  | { type: 'send'; conversationId: string; text: string; id: string; now: number }
  | { type: 'stop' | 'retry'; taskId: string; now: number }
  | { type: 'approve' | 'reject'; taskId: string; approvalId: string }
  | { type: 'settings'; value: Partial<AppState['settings']> }
  | {
      type: 'connect' | 'disconnect';
      connectionId: string;
      account?: string;
      simulateFailure?: boolean;
    }
  | { type: 'note-save'; note: Note }
  | { type: 'note-delete'; id: string }
  | { type: 'read-notification'; id: string };

function notify(state: AppState, notification: Notification): AppState {
  if (
    !state.settings.notifications ||
    state.notifications.some((n) => n.eventKey === notification.eventKey)
  )
    return state;
  return { ...state, notifications: [notification, ...state.notifications] };
}

function taskNotice(state: AppState, task: Task, kind: Notification['kind']): AppState {
  return notify(state, {
    id: `${task.id}:${task.attempt}:${kind}`,
    eventKey: `${task.id}:${task.attempt}:${kind}`,
    kind,
    read: false,
    conversationId: task.conversationId,
    taskId: task.id,
    title: task.title,
    detail: say(
      task.language,
      {
        completed: '已完成，成果准备好了',
        failed: '任务遇到了问题',
        approval: '有一个操作需要确认',
        connection: '连接状态已更新',
      }[kind],
      {
        completed: 'Finished. Your results are ready.',
        failed: 'This task needs attention.',
        approval: 'An action needs your approval.',
        connection: 'Connection updated.',
      }[kind],
    ),
  });
}

function updateTask(state: AppState, task: Task): AppState {
  return { ...state, tasks: state.tasks.map((t) => (t.id === task.id ? task : t)) };
}

export function command(state: AppState, action: Command): AppState {
  const lang = state.settings.language;
  switch (action.type) {
    case 'select':
      if (!state.conversations.some((c) => c.id === action.id)) return state;
      return {
        ...state,
        selectedId: action.id,
        conversations: state.conversations.map((c) =>
          c.id === action.id ? { ...c, unread: 0 } : c,
        ),
      };
    case 'new': {
      const id = conversationKey('desktop', action.id);
      if (state.conversations.some((c) => c.id === id)) return state;
      return {
        ...state,
        selectedId: id,
        conversations: [
          {
            id,
            connectionId: 'desktop',
            locator: action.id,
            requesterId: requesterKey('desktop', 'you'),
            sessionId: action.id,
            name: say(lang, '新的对话', 'New conversation'),
            origin: 'desktop',
            unread: 0,
          },
          ...state.conversations,
        ],
      };
    }
    case 'rename': {
      const name = action.name.trim();
      if (!canControl(state, action.conversationId) || !name || name.length > 80) return state;
      return {
        ...state,
        conversations: state.conversations.map((c) =>
          c.id === action.conversationId ? { ...c, name, customName: true } : c,
        ),
      };
    }
    case 'draft':
      return canControl(state, action.conversationId)
        ? { ...state, drafts: { ...state.drafts, [action.conversationId]: action.text } }
        : state;
    case 'send': {
      const c = state.conversations.find((c) => c.id === action.conversationId);
      const text = action.text.trim();
      if (
        !c ||
        !canControl(state, c.id) ||
        !text ||
        state.tasks.some(
          (t) => t.id === action.id || (t.conversationId === c.id && active(t.status)),
        )
      )
        return state;
      const kind: Task['kind'] = /写|文件|整理|修改|write|file|edit|organize/i.test(text)
        ? 'file'
        : /计划|旅行|对比|调研|小队|比较|plan|research|team|compare|trip/i.test(text)
          ? 'team'
          : 'chat';
      const task: Task = {
        id: action.id,
        conversationId: c.id,
        requesterId: c.requesterId,
        sessionId: c.sessionId,
        title: text.slice(0, 60),
        kind,
        status: 'queued',
        step: 0,
        attempt: 1,
        createdAt: action.now,
        language: lang,
        shouldFail: /失败|故障|fail|error/i.test(text),
        history: [],
        members:
          kind === 'team'
            ? [
                {
                  id: 'explorer',
                  name: 'Scout',
                  role: say(lang, '资料搜集', 'Explore'),
                  state: 'waiting',
                  result: '',
                },
                {
                  id: 'planner',
                  name: 'Moss',
                  role: say(lang, '分析与规划', 'Make a plan'),
                  state: 'waiting',
                  result: '',
                },
                {
                  id: 'reviewer',
                  name: 'Fern',
                  role: say(lang, '检查与汇总', 'Review'),
                  state: 'waiting',
                  result: '',
                },
              ]
            : [],
        approval: null,
        artifact: null,
      };
      return {
        ...state,
        tasks: [task, ...state.tasks],
        drafts: { ...state.drafts, [c.id]: '' },
        conversations: state.conversations.map((v) =>
          v.id === c.id && !v.customName && !state.messages.some((m) => m.conversationId === c.id)
            ? { ...v, name: text.slice(0, 20) }
            : v,
        ),
        messages: [
          ...state.messages,
          {
            id: `${action.id}:user`,
            conversationId: c.id,
            sessionId: c.sessionId,
            role: 'user',
            text,
            createdAt: action.now,
            state: 'complete',
          },
        ],
      };
    }
    case 'stop':
    case 'retry':
    case 'approve':
    case 'reject': {
      const task = state.tasks.find((t) => t.id === action.taskId);
      if (!task || !canControl(state, task.conversationId)) return state;
      if (action.type === 'stop') {
        if (!active(task.status) || task.status === 'cancelling') return state;
        return updateTask(state, {
          ...task,
          status: 'cancelling',
          approval:
            task.approval?.status === 'pending'
              ? { ...task.approval, status: 'cancelled' }
              : task.approval,
        });
      }
      if (action.type === 'retry') {
        if (
          !['failed', 'cancelled', 'interrupted'].includes(task.status) ||
          state.tasks.some((t) => t.conversationId === task.conversationId && active(t.status))
        )
          return state;
        return updateTask(state, {
          ...task,
          attempt: task.attempt + 1,
          step: 0,
          createdAt: action.now,
          status: 'queued',
          shouldFail: false,
          history: [...task.history, { attempt: task.attempt, status: task.status }],
          approval: null,
          artifact: null,
          members: task.members.map((m) => ({ ...m, state: 'waiting', result: '' })),
        });
      }
      if (
        !('approvalId' in action) ||
        task.status !== 'awaiting_approval' ||
        !task.approval ||
        task.approval.id !== action.approvalId ||
        task.approval.status !== 'pending'
      )
        return state;
      if (action.type === 'approve') {
        if (state.settings.permission === 'deny') return state;
        return updateTask(state, {
          ...task,
          status: 'running',
          approval: { ...task.approval, status: 'approved' },
        });
      }
      return taskNotice(
        updateTask(state, {
          ...task,
          status: 'failed',
          approval: { ...task.approval, status: 'rejected' },
        }),
        task,
        'failed',
      );
    }
    case 'settings':
      return { ...state, settings: { ...state.settings, ...action.value } };
    case 'connect':
    case 'disconnect':
      return {
        ...state,
        connections: state.connections.map((c) =>
          c.id === action.connectionId
            ? {
                ...c,
                status: action.type === 'connect' ? 'connecting' : 'disconnected',
                attempt: c.attempt + 1,
                account: action.account ?? c.account,
                simulateFailure: action.simulateFailure ?? c.simulateFailure,
              }
            : c,
        ),
      };
    case 'note-save': {
      const old = state.notes.find((n) => n.id === action.note.id);
      if (
        !old ||
        old.scope !== action.note.scope ||
        old.ownerId !== action.note.ownerId ||
        !action.note.title.trim() ||
        !action.note.body.trim()
      )
        return state;
      return { ...state, notes: state.notes.map((n) => (n.id === old.id ? action.note : n)) };
    }
    case 'note-delete':
      return { ...state, notes: state.notes.filter((n) => n.id !== action.id) };
    case 'read-notification':
      return {
        ...state,
        notifications: state.notifications.map((n) =>
          n.id === action.id ? { ...n, read: true } : n,
        ),
      };
  }
}

const memoryCue = /记住|remember|我喜欢|我偏好|我习惯|I prefer|I like/i;

export function responseFor(task: Task, memoryEnabled = true): string {
  const zh = task.language === 'zh';
  if (task.kind === 'chat' && memoryCue.test(task.title)) {
    return memoryEnabled
      ? say(
          task.language,
          '好的，这条信息会作为你的个人偏好保留。你可以在「记忆」中查看、修改或删除它，其他联系人的偏好仍然分别保存。',
          'I’ll keep this as a personal preference. You can review, edit or remove it in Memory. Other people’s preferences stay separate.',
        )
      : say(
          task.language,
          '自动记忆目前已关闭。这条信息会留在本次对话中，不会新增长期记忆。',
          'Auto memory is currently off. This stays in our conversation without creating a new long-term note.',
        );
  }
  if (task.kind === 'file')
    return zh
      ? '已经整理好一份清晰的文件方案。\n\n- **工作资料**：项目文档、会议笔记与参考内容\n- **生活记录**：旅行计划、阅读摘录与灵感\n- **待处理**：尚未归类的文件，留待你确认\n\n右侧可以查看方案和变更预览。原始资料会保留，方便回看。'
      : 'Your file plan is ready.\n\n- **Work**: project documents, meeting notes and references\n- **Life**: travel plans, reading notes and ideas\n- **To sort**: anything that still needs your decision\n\nOpen the artifact to review the plan. Original materials are kept for reference.';
  if (task.kind === 'team')
    return zh
      ? '小队已经把这件事梳理好了。\n\nScout 整理了相关资料，Moss 对比了可选方案，Fern 完成了交叉检查。\n\n**建议先从最重要的一步开始**，把需要进一步确认的事项留在清单里。详细方案已放在右侧，随时可以继续讨论。'
      : 'The team has put a plan together.\n\nScout gathered the references, Moss compared the options, and Fern checked the result.\n\n**Start with the most important next step**, and keep open questions in the checklist. Your plan is ready to review on the right.';
  return zh
    ? '我在。可以把想法、资料或者手头的事情直接告诉我。\n\n我们可以先理清目标，再一起把下一步做具体。如果需要多人分工，我会组织小队，并把过程和结果留在这里。'
    : 'I’m here. Bring an idea, a document, or whatever is on your mind.\n\nWe can work out the goal and make the next step concrete. When a task benefits from a team, I’ll bring in a few helpers and keep the progress here.';
}

export function advanceDemo(
  state: AppState,
  taskId: string,
  attempt: number,
  now: number,
): AppState {
  const old = state.tasks.find((t) => t.id === taskId);
  if (!old || old.attempt !== attempt || !active(old.status) || old.status === 'awaiting_approval')
    return state;
  const c = state.conversations.find((c) => c.id === old.conversationId)!;
  if (
    c.origin === 'channel' &&
    !state.connections.some((v) => v.id === c.connectionId && v.status === 'connected')
  )
    return state;
  if (old.status === 'cancelling')
    return {
      ...updateTask(state, {
        ...old,
        status: 'cancelled',
        members: old.members.map((m) =>
          m.state === 'done' || m.state === 'failed' ? m : { ...m, state: 'stopped' },
        ),
      }),
      messages: state.messages.map((m) =>
        m.id === `${old.id}:${attempt}:reply` ? { ...m, state: 'interrupted' } : m,
      ),
    };
  let task: Task = { ...old, status: 'running', step: old.step + 1 };
  task.members = task.members.map((m, i) => ({
    ...m,
    state: task.step > i + 3 ? 'done' : task.step >= Math.min(i + 1, 2) ? 'working' : 'waiting',
    result:
      task.step > i + 3
        ? say(
            task.language,
            '已整理并核对，结果交给主助手。',
            'Checked and passed back to your assistant.',
          )
        : '',
  }));
  if (task.kind === 'file' && !task.approval && task.step >= 3) {
    const permission = c.origin === 'desktop' ? state.settings.permission : 'ask';
    if (permission === 'deny')
      return taskNotice(updateTask(state, { ...task, status: 'failed' }), task, 'failed');
    if (permission === 'ask') {
      task = {
        ...task,
        status: 'awaiting_approval',
        approval: {
          id: `${task.id}:${attempt}:approval`,
          status: 'pending',
          target: 'workspace/notes/organization.md',
        },
      };
      return taskNotice(updateTask(state, task), task, 'approval');
    }
  }
  if (task.shouldFail && task.step >= 4) {
    task = {
      ...task,
      status: 'failed',
      members: task.members.map((m, i) =>
        i === 0
          ? {
              ...m,
              state: 'failed',
              result: say(
                task.language,
                '示例资料暂时不可用。',
                'The sample reference is temporarily unavailable.',
              ),
            }
          : { ...m, state: 'stopped' },
      ),
    };
    return taskNotice(updateTask(state, task), task, 'failed');
  }
  let next = updateTask(state, task);
  if (task.step < 5) return next;
  const id = `${task.id}:${attempt}:reply`;
  const existing = state.messages.find((m) => m.id === id);
  const fullText = responseFor(task, state.settings.memory);
  const text = fullText.slice(0, (existing?.text.length ?? 0) + 60);
  const done = text.length === fullText.length;
  const reply = {
    id,
    conversationId: c.id,
    sessionId: task.sessionId,
    role: 'assistant' as const,
    text,
    createdAt: existing?.createdAt ?? now,
    state: done ? ('complete' as const) : ('streaming' as const),
  };
  next = {
    ...next,
    messages: existing
      ? next.messages.map((m) => (m.id === id ? reply : m))
      : [...next.messages, reply],
  };
  if (!done) return next;
  task = {
    ...task,
    status: 'completed',
    members: task.members.map((m) => ({ ...m, state: 'done' })),
    artifact:
      task.kind === 'chat'
        ? null
        : {
            name: task.kind === 'file' ? 'organization.md' : 'thoughtful-plan.md',
            kind: 'markdown',
            body: `# ${task.title}\n\n${fullText}\n\n---\n\n${say(task.language, '## 下一步\n\n- [ ] 确认优先事项\n- [ ] 补充需要的资料\n- [ ] 开始第一步\n\n> 来源：Orangutan 演示工作区。', '## Next steps\n\n- [ ] Confirm priorities\n- [ ] Add the missing references\n- [ ] Take the first step\n\n> Source: Orangutan demo workspace.')}`,
          },
  };
  next = updateTask(next, task);
  next = {
    ...next,
    conversations: next.conversations.map((v) =>
      v.id === c.id && c.id !== next.selectedId ? { ...v, unread: v.unread + 1 } : v,
    ),
  };
  const userText = next.messages.find((m) => m.id === `${task.id}:user`)?.text ?? '';
  if (
    next.settings.memory &&
    memoryCue.test(userText) &&
    !next.notes.some((n) => n.id === `${task.id}:memory`)
  ) {
    next = {
      ...next,
      notes: [
        ...next.notes,
        {
          id: `${task.id}:memory`,
          scope: 'personal',
          ownerId: c.requesterId,
          title: userText.slice(0, 30),
          body: userText,
          source: c.name,
          updatedAt: now,
        },
      ],
    };
  }
  return taskNotice(next, task, 'completed');
}

export function settleConnection(state: AppState, id: string, attempt: number): AppState {
  const c = state.connections.find((c) => c.id === id);
  if (!c || c.attempt !== attempt || c.status !== 'connecting') return state;
  const failed = c.simulateFailure;
  const next: AppState = {
    ...state,
    connections: state.connections.map((v) =>
      v.id === id ? { ...v, status: failed ? 'error' : 'connected' } : v,
    ),
  };
  return failed
    ? notify(next, {
        id: `${id}:${attempt}:error`,
        eventKey: `${id}:${attempt}:error`,
        kind: 'connection',
        conversationId: null,
        taskId: null,
        read: false,
        title: c.name,
        detail: say(
          state.settings.language,
          '示例连接失败，请检查配置后重试。',
          'Sample connection failed. Check the configuration and try again.',
        ),
      })
    : next;
}

export function recover(state: AppState): AppState {
  return {
    ...state,
    connections: state.connections.map((c) => ({
      ...c,
      status: 'disconnected',
      attempt: c.attempt + 1,
    })),
    tasks: state.tasks.map((t) => ({
      ...t,
      status: active(t.status) ? 'interrupted' : t.status,
      members: t.members.map((m) =>
        t.status !== 'completed' && (m.state === 'working' || m.state === 'waiting')
          ? { ...m, state: 'stopped' }
          : m,
      ),
      approval:
        t.approval?.status === 'pending' ? { ...t.approval, status: 'expired' } : t.approval,
    })),
    messages: state.messages.map((m) =>
      m.state === 'streaming' ? { ...m, state: 'interrupted' } : m,
    ),
  };
}
