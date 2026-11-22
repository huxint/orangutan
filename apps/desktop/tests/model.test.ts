import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createInitialState, HOME } from '../src/fixtures.ts';
import { command, advanceDemo, settleConnection, recover, active } from '../src/model.ts';
import { conversationKey, stateSchema, type AppState } from '../src/schema.ts';
import { loadState, resetState, saveState, STORAGE_KEY } from '../src/storage.ts';

const start = (text = '帮我整理文件', state = createInitialState(1000), id = 'local-task') =>
  command(state, { type: 'send', id, conversationId: HOME, text, now: 2000 });
function run(state: AppState, id = 'local-task', steps = 20): AppState {
  for (let i = 0; i < steps; i++)
    state = advanceDemo(state, id, state.tasks.find((t) => t.id === id)!.attempt, 3000 + i);
  return state;
}

test('source keys preserve account and chat identity; invalid snapshots fail validation', () => {
  const state = createInitialState(1000);
  assert.equal(stateSchema.safeParse(state).success, true);
  const chats = state.conversations.filter((c) => c.name.startsWith('周然'));
  assert.notEqual(chats[0].id, chats[1].id);
  assert.notEqual(chats[0].requesterId, chats[1].requesterId);
  assert.notEqual(conversationKey('a:b', 'c'), conversationKey('a', 'b:c'));
  const invalid = structuredClone(state);
  invalid.tasks[0].requesterId = 'someone-else';
  assert.equal(stateSchema.safeParse(invalid).success, false);
  invalid.tasks[0] = state.tasks[0];
  invalid.conversations[0].origin = 'channel';
  assert.equal(stateSchema.safeParse(invalid).success, false);
});

test('desktop commands cannot mutate channel conversations, tasks or approvals', () => {
  const state = createInitialState(1000);
  const task = state.tasks.find((t) => t.id === 'qq-files')!;
  const actions = [
    { type: 'send', conversationId: task.conversationId, id: 'forbidden', text: 'write', now: 0 },
    { type: 'draft', conversationId: task.conversationId, text: 'forbidden' },
    { type: 'rename', conversationId: task.conversationId, name: 'forbidden' },
    { type: 'stop', taskId: task.id, now: 0 },
    { type: 'retry', taskId: task.id, now: 0 },
    { type: 'approve', taskId: task.id, approvalId: task.approval!.id },
    { type: 'reject', taskId: task.id, approvalId: task.approval!.id },
  ] as const;
  for (const action of actions) assert.strictEqual(command(state, action), state);
  const changed = command(state, { type: 'settings', value: { permission: 'allow' } });
  assert.deepEqual(changed.tasks, state.tasks);
});

test('local approvals bind exact requests, reject effects and cannot be replayed', () => {
  let state = run(start());
  const task = state.tasks.find((t) => t.id === 'local-task')!;
  assert.equal(task.status, 'awaiting_approval');
  assert.equal(task.artifact, null);
  assert.strictEqual(
    command(state, { type: 'approve', taskId: task.id, approvalId: 'different' }),
    state,
  );
  state = command(state, { type: 'reject', taskId: task.id, approvalId: task.approval!.id });
  state = run(state);
  assert.equal(state.tasks[0].status, 'failed');
  assert.equal(state.tasks[0].artifact, null);
  assert.equal(
    state.messages.filter((m) => m.role === 'assistant' && m.conversationId === HOME).length,
    0,
  );
  assert.strictEqual(
    command(state, { type: 'approve', taskId: task.id, approvalId: task.approval!.id }),
    state,
  );
});

test('allow, ask and deny have distinct file execution behavior', () => {
  let ask = run(start());
  ask = command(ask, {
    type: 'approve',
    taskId: 'local-task',
    approvalId: ask.tasks[0].approval!.id,
  });
  ask = run(ask);
  assert.equal(ask.tasks[0].status, 'completed');
  assert.ok(ask.tasks[0].artifact?.body);
  const allow = run(
    start(
      'write notes',
      command(createInitialState(), { type: 'settings', value: { permission: 'allow' } }),
    ),
  );
  assert.equal(allow.tasks[0].status, 'completed');
  assert.equal(allow.tasks[0].approval, null);
  const deny = run(
    start(
      'write notes',
      command(createInitialState(), { type: 'settings', value: { permission: 'deny' } }),
    ),
  );
  assert.equal(deny.tasks[0].status, 'failed');
  assert.equal(deny.tasks[0].artifact, null);
});

test('one in-flight run per session; switching views does not redirect output', () => {
  let state = start('hello');
  assert.strictEqual(start('second request', state, 'second'), state);
  state = command(state, {
    type: 'select',
    id: state.conversations.find((c) => c.connectionId === 'telegram-main')!.id,
  });
  state = run(state);
  const reply = state.messages.find((m) => m.id === 'local-task:1:reply')!;
  assert.equal(reply.conversationId, HOME);
  assert.equal(state.tasks[0].status, 'completed');
});

test('cancelled attempts stay terminal and a retry preserves history', () => {
  let state = run(start('team plan'), 'local-task', 5);
  state = command(state, { type: 'stop', taskId: 'local-task', now: 4000 });
  assert.equal(state.tasks[0].status, 'cancelling');
  state = advanceDemo(state, 'local-task', 1, 4001);
  assert.equal(state.tasks[0].status, 'cancelled');
  assert.ok(state.tasks[0].members.every((m) => m.state !== 'working' && m.state !== 'waiting'));
  assert.strictEqual(advanceDemo(state, 'local-task', 1, 4002), state);
  state = command(state, { type: 'retry', taskId: 'local-task', now: 5000 });
  assert.deepEqual(state.tasks[0].history, [{ attempt: 1, status: 'cancelled' }]);
  assert.strictEqual(advanceDemo(state, 'local-task', 1, 5001), state);
  state = run(state);
  assert.equal(state.tasks[0].attempt, 2);
  assert.equal(state.tasks[0].status, 'completed');
  assert.equal(state.messages.find((m) => m.id === 'local-task:1:reply')!.state, 'interrupted');
});

test('team failures are visible, and explicit retries can complete', () => {
  let state = run(start('team plan with error'));
  assert.equal(state.tasks[0].status, 'failed');
  assert.equal(state.tasks[0].members[0].state, 'failed');
  state = command(state, { type: 'retry', taskId: 'local-task', now: 5000 });
  state = run(state);
  assert.equal(state.tasks[0].status, 'completed');
  assert.ok(state.tasks[0].members.every((m) => m.state === 'done'));
});

test('notifications deduplicate and marking read does not touch task or message state', () => {
  let state = run(start('hello'));
  const notices = state.notifications.filter((n) => n.taskId === 'local-task');
  assert.equal(notices.length, 1);
  state = run(state);
  assert.equal(state.notifications.filter((n) => n.taskId === 'local-task').length, 1);
  const read = command(state, { type: 'read-notification', id: notices[0].id });
  assert.deepEqual(read.tasks, state.tasks);
  assert.deepEqual(read.messages, state.messages);
  assert.equal(read.notifications.find((n) => n.id === notices[0].id)!.read, true);
});

test('channel observations update only their source and can finish without desktop control', () => {
  let state = command(createInitialState(), { type: 'connect', connectionId: 'qq-studio' });
  const connection = state.connections.find((c) => c.id === 'qq-studio')!;
  state = settleConnection(state, connection.id, connection.attempt);
  state = run(state, 'studio-notes');
  const task = state.tasks.find((t) => t.id === 'studio-notes')!;
  assert.equal(task.status, 'completed');
  assert.equal(state.messages.filter((m) => m.conversationId === HOME).length, 0);
  assert.equal(
    state.notifications.find((n) => n.taskId === task.id)!.conversationId,
    task.conversationId,
  );
  assert.equal(
    state.messages.find((m) => m.id === 'studio-notes:1:reply')!.conversationId,
    task.conversationId,
  );
});

test('connection changes cannot approve or cancel tasks; stale connection events are ignored', () => {
  const before = createInitialState();
  let state = command(before, { type: 'disconnect', connectionId: 'qq-main' });
  assert.deepEqual(state.tasks, before.tasks);
  state = command(state, { type: 'connect', connectionId: 'qq-main', simulateFailure: true });
  const attempt = state.connections.find((c) => c.id === 'qq-main')!.attempt;
  assert.strictEqual(settleConnection(state, 'qq-main', attempt - 1), state);
  state = settleConnection(state, 'qq-main', attempt);
  assert.equal(state.connections.find((c) => c.id === 'qq-main')!.status, 'error');
  assert.deepEqual(state.tasks, before.tasks);
});

test('memory has explicit scopes, respects its switch and preserves unrelated records', () => {
  const automatic = run(start('我喜欢慢节奏的安排'));
  assert.equal(automatic.notes.find((n) => n.id === 'local-task:memory')?.scope, 'personal');
  let state = run(start('记住我喜欢慢慢读书'));
  const note = state.notes.find((n) => n.id === 'local-task:memory')!;
  assert.equal(note.scope, 'personal');
  assert.equal(note.ownerId, state.conversations.find((c) => c.id === HOME)!.requesterId);
  assert.strictEqual(
    command(state, { type: 'note-save', note: { ...note, scope: 'shared', ownerId: null } }),
    state,
  );
  const before = state.messages;
  state = command(state, { type: 'note-delete', id: note.id });
  assert.deepEqual(state.messages, before);
  const disabled = run(
    start(
      'remember that I like tea',
      command(createInitialState(), { type: 'settings', value: { memory: false } }),
    ),
  );
  assert.equal(disabled.notes.length, createInitialState().notes.length);
});

test('reopening marks active work interrupted, expires approvals and disconnects transports', () => {
  const old = run(start());
  const state = recover(old);
  assert.equal(state.tasks[0].status, 'interrupted');
  assert.equal(state.tasks[0].approval?.status, 'expired');
  assert.ok(state.connections.every((c) => c.status === 'disconnected'));
  assert.ok(state.tasks.every((t) => !active(t.status)));
  assert.deepEqual(
    state.tasks.find((t) => t.id === 'reading-plan')!.artifact,
    old.tasks.find((t) => t.id === 'reading-plan')!.artifact,
  );
  assert.strictEqual(advanceDemo(state, 'local-task', 1, 9000), state);
});

test('storage rejects corrupt snapshots without overwriting and backs up explicit resets', () => {
  const entries = new Map([[STORAGE_KEY, '{broken']]);
  const storage = {
    getItem: (key: string) => entries.get(key) ?? null,
    setItem: (key: string, value: string) => {
      entries.set(key, value);
    },
  };
  assert.equal(loadState(storage).blocked, true);
  assert.equal(entries.get(STORAGE_KEY), '{broken');
  const reset = resetState(storage, 12);
  assert.ok(reset);
  assert.equal(entries.get(`${STORAGE_KEY}.backup.12`), '{broken');
  assert.equal(loadState(storage).blocked, false);
  const unavailable = {
    getItem: () => {
      throw new Error('denied');
    },
    setItem: () => {
      throw new Error('full');
    },
  };
  assert.equal(loadState(unavailable).blocked, true);
  assert.equal(saveState(unavailable, createInitialState()), false);
  assert.equal(resetState(unavailable), null);
});

test('custom conversation names survive first send and recovery without changing identities', () => {
  const original = createInitialState(1000);
  for (const name of ['', '   ', 'x'.repeat(81)]) {
    assert.strictEqual(command(original, { type: 'rename', conversationId: HOME, name }), original);
  }
  assert.strictEqual(
    command(original, { type: 'rename', conversationId: 'missing', name: 'Name' }),
    original,
  );
  const renamed = command(original, { type: 'rename', conversationId: HOME, name: '  产品设计  ' });
  assert.strictEqual(renamed.messages, original.messages);
  assert.strictEqual(renamed.tasks, original.tasks);
  const before = original.conversations.find((c) => c.id === HOME)!;
  assert.deepEqual(
    renamed.conversations.find((c) => c.id === HOME),
    { ...before, name: '产品设计', customName: true },
  );
  const restored = recover(stateSchema.parse(JSON.parse(JSON.stringify(renamed))));
  const sent = start('讨论下一步', restored);
  assert.equal(sent.conversations.find((c) => c.id === HOME)!.name, '产品设计');
  assert.equal(stateSchema.safeParse(original).success, true);
});
