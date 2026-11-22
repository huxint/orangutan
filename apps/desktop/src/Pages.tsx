import { useState } from 'react';
import {
  ArrowDownToLine,
  Cable,
  ChevronRight,
  FileText,
  FolderOpen,
  Globe2,
  Leaf,
  LockKeyhole,
  Monitor,
  Moon,
  Pencil,
  Settings2,
  ShieldCheck,
  SlidersHorizontal,
  Sun,
  Trash2,
  Users,
  Wifi,
  WifiOff,
} from 'lucide-react';
import { useApp } from './context.ts';
import { active } from './model.ts';
import {
  Empty,
  Logo,
  Modal,
  PlatformIcon,
  Source,
  StatusBadge,
  Switch,
  Time,
} from './components.tsx';
import { SearchField, Segmented, SettingRow } from './controls.tsx';
import { PageFrame, Toolbar } from './layout.tsx';
import type { Connection, Note } from './schema.ts';

const taskFilters = {
  all: () => true,
  active,
  completed: (status: string) => status === 'completed',
  other: (status: string) => ['failed', 'interrupted', 'cancelled'].includes(status),
};

export function TasksPage() {
  const { state, t, openTask } = useApp();
  const [filter, setFilter] = useState<keyof typeof taskFilters>('all');
  const [source, setSource] = useState('all');
  const [query, setQuery] = useState('');
  const origin = (conversationId: string) =>
    state.conversations.find((c) => c.id === conversationId)?.origin;
  const tasks = state.tasks.filter(
    (task) =>
      taskFilters[filter](task.status) &&
      (source === 'all' || origin(task.conversationId) === source) &&
      task.title.toLocaleLowerCase().includes(query.trim().toLocaleLowerCase()),
  );
  const count = (key: keyof typeof taskFilters) =>
    state.tasks.filter((task) => taskFilters[key](task.status)).length;
  return (
    <PageFrame
      title={t('任务', 'Tasks')}
      detail={t(
        '查看执行状态、待确认操作和历史结果。',
        'Review execution status, pending actions and completed results.',
      )}
    >
      <Toolbar>
        <Segmented
          label={t('任务状态', 'Task status')}
          value={filter}
          onChange={(value) => setFilter(value as keyof typeof taskFilters)}
          options={[
            { value: 'all', label: t('全部', 'All'), count: count('all') },
            { value: 'active', label: t('进行中', 'In progress'), count: count('active') },
            { value: 'completed', label: t('已完成', 'Completed'), count: count('completed') },
            { value: 'other', label: t('需关注', 'Needs attention'), count: count('other') },
          ]}
        />
        <SearchField
          label={t('搜索任务', 'Search tasks')}
          placeholder={t('找一件事…', 'Find a task…')}
          value={query}
          onChange={setQuery}
        />
        <select
          aria-label={t('任务来源', 'Task source')}
          value={source}
          onChange={(e) => setSource(e.target.value)}
        >
          <option value="all">{t('所有入口', 'All sources')}</option>
          <option value="desktop">{t('桌面', 'Desktop')}</option>
          <option value="channel">{t('消息渠道', 'Channels')}</option>
        </select>
      </Toolbar>
      {tasks.length > 0 && (
        <div className="task-table">
          <div className="task-table-heading" aria-hidden="true">
            <span>{t('任务', 'Task')}</span>
            <span>{t('状态', 'Status')}</span>
            <span />
          </div>
          {tasks.map((task) => (
            <button
              className="task-row"
              key={task.id}
              onClick={() => openTask(task.id)}
              data-testid={`task-row-${task.id}`}
            >
              <span className="task-row-main">
                <span className="activity-icon">
                  {task.kind === 'team' ? <Users size={16} /> : <FileText size={16} />}
                </span>
                <span>
                  <strong>{task.title}</strong>
                  <small>
                    <Source conversationId={task.conversationId} />
                    {' · '}
                    <Time value={task.createdAt} format="datetime" />
                  </small>
                </span>
              </span>
              <StatusBadge status={task.status} />
              <ChevronRight size={15} className="row-chevron" />
            </button>
          ))}
        </div>
      )}
      {!tasks.length && (
        <Empty
          title={t('暂无匹配任务', 'No matching tasks')}
          detail={t(
            '换个筛选条件，或者在对话中交代一件新事情。',
            'Try a different filter, or start something in a conversation.',
          )}
        />
      )}
      <div className="page-note">
        <LockKeyhole size={14} />
        {t(
          '来自其他渠道的任务可以查看，操作请回到原渠道。',
          'Channel tasks are here to view. Take action in their original channel.',
        )}
      </div>
    </PageFrame>
  );
}

export function ConnectionsPage() {
  const { state, dispatch, t, selectConversation } = useApp();
  const [editing, setEditing] = useState<Connection | null>(null);
  const [account, setAccount] = useState('');
  const [simulateFailure, setSimulateFailure] = useState(false);
  const labels = {
    connected: t('已连接', 'Connected'),
    connecting: t('连接中', 'Connecting'),
    disconnected: t('未连接', 'Disconnected'),
    error: t('连接异常', 'Connection error'),
  };
  return (
    <PageFrame
      title={t('连接', 'Connections')}
      detail={t(
        '管理消息渠道连接。各渠道的会话与权限独立。',
        'Manage channel connections. Conversations and permissions remain separate.',
      )}
    >
      <div className="connection-list">
        {state.connections.map((connection) => {
          const conversations = state.conversations.filter((c) => c.connectionId === connection.id);
          return (
            <article className="connection-card" key={connection.id}>
              <PlatformIcon platform={connection.platform} size={18} />
              <div className="connection-main">
                <h2>
                  <button
                    disabled={!conversations.length}
                    title={t('查看会话', 'View conversations')}
                    onClick={() => selectConversation(conversations[0].id)}
                  >
                    {connection.name}
                  </button>
                </h2>
                <p>
                  {connection.account}
                  <span>
                    {t(
                      `${conversations.length} 段独立会话`,
                      `${conversations.length} ${conversations.length === 1 ? 'conversation' : 'conversations'}`,
                    )}
                  </span>
                </p>
              </div>
              <span className={`connection-status ${connection.status}`}>
                <span />
                {labels[connection.status]}
              </span>
              <div className="connection-actions">
                <button
                  className="button secondary small"
                  onClick={() => {
                    setEditing(connection);
                    setAccount(connection.account);
                    setSimulateFailure(connection.simulateFailure);
                  }}
                >
                  <Settings2 size={13} />
                  {t('配置', 'Configure')}
                </button>
                {connection.status === 'connected' || connection.status === 'connecting' ? (
                  <button
                    className="button ghost small"
                    onClick={() => dispatch({ type: 'disconnect', connectionId: connection.id })}
                  >
                    <WifiOff size={13} />
                    {t('断开', 'Disconnect')}
                  </button>
                ) : (
                  <button
                    className="button primary small"
                    onClick={() => dispatch({ type: 'connect', connectionId: connection.id })}
                  >
                    <Wifi size={13} />
                    {t('连接', 'Connect')}
                  </button>
                )}
              </div>
            </article>
          );
        })}
      </div>
      <div className="page-callout">
        <Cable size={17} />
        <div>
          <h3>{t('共享工作区，会话隔离', 'Shared workspace, isolated conversations')}</h3>
          <p>
            {t(
              '共享工作区和公共记忆，让不同入口的助手有相同的基础。个人偏好和聊天上下文各自保存。应用关闭后，连接和执行一起停止。',
              'Your workspace and shared memory provide common ground. Personal preferences and conversation context stay separate. Closing the app stops connections and execution.',
            )}
          </p>
        </div>
      </div>
      {editing && (
        <Modal
          title={t('配置演示连接', 'Configure demo connection')}
          close={() => setEditing(null)}
        >
          <div className="modal-body">
            <div className="connection-modal-brand">
              <PlatformIcon platform={editing.platform} size={18} />
              <span>
                <strong>{editing.name}</strong>
                <small>{t('演示连接', 'Demo connection')}</small>
              </span>
            </div>
            <label className="form-field">
              {t('示例账号名称', 'Sample account name')}
              <input value={account} onChange={(e) => setAccount(e.target.value)} />
            </label>
            <label className="form-field">
              {t('演示连接结果', 'Demo connection result')}
              <select
                value={simulateFailure ? 'error' : 'success'}
                onChange={(e) => setSimulateFailure(e.target.value === 'error')}
              >
                <option value="success">{t('连接成功', 'Successful connection')}</option>
                <option value="error">{t('连接异常', 'Connection error')}</option>
              </select>
            </label>
            <p className="field-help">
              {t(
                '这里只保存演示配置，不会连接真实账号，也不需要密钥。',
                'This saves a demo configuration. No real account is connected and no key is needed.',
              )}
            </p>
            <div className="modal-actions">
              <button className="button secondary" onClick={() => setEditing(null)}>
                {t('取消', 'Cancel')}
              </button>
              <button
                className="button primary"
                disabled={!account.trim()}
                onClick={() => {
                  dispatch({
                    type: 'connect',
                    connectionId: editing.id,
                    account: account.trim(),
                    simulateFailure,
                  });
                  setEditing(null);
                }}
              >
                {t('保存并模拟连接', 'Save and simulate connection')}
              </button>
            </div>
          </div>
        </Modal>
      )}
    </PageFrame>
  );
}

export function MemoryPage() {
  const { state, dispatch, t, toast } = useApp();
  const [query, setQuery] = useState('');
  const [scope, setScope] = useState('all');
  const [source, setSource] = useState('all');
  const [editing, setEditing] = useState<Note | null>(null);
  const [deleting, setDeleting] = useState(false);
  const needle = query.trim().toLocaleLowerCase();
  const notes = state.notes.filter(
    (n) =>
      (scope === 'all' || n.scope === scope) &&
      (source === 'all' || n.source === source) &&
      `${n.title} ${n.body}`.toLocaleLowerCase().includes(needle),
  );
  return (
    <PageFrame
      title={t('记忆', 'Memory')}
      detail={t(
        '管理共享知识和个人偏好，检查每条记录的来源与作用域。',
        'Manage shared knowledge and personal preferences. Review each record’s source and scope.',
      )}
      action={
        <Switch
          checked={state.settings.memory}
          label={t('自动记忆', 'Auto memory')}
          showLabel
          onChange={(memory) => dispatch({ type: 'settings', value: { memory } })}
        />
      }
    >
      {!state.settings.memory && (
        <div className="notice">
          {t(
            '自动记忆已关闭，已有记录会继续保留。',
            'Auto memory is off. Your existing notes are kept.',
          )}
        </div>
      )}
      <Toolbar>
        <Segmented
          label={t('记忆作用域', 'Memory scope')}
          value={scope}
          onChange={setScope}
          options={[
            { value: 'all', label: t('全部记忆', 'All memories'), count: state.notes.length },
            {
              value: 'shared',
              label: t('共享基础', 'Shared'),
              count: state.notes.filter((n) => n.scope === 'shared').length,
            },
            {
              value: 'personal',
              label: t('个人记忆', 'Personal'),
              count: state.notes.filter((n) => n.scope === 'personal').length,
            },
          ]}
        />
        <SearchField
          label={t('搜索记忆', 'Search memories')}
          placeholder={t('搜索记忆…', 'Search memories…')}
          value={query}
          onChange={setQuery}
        />
        <select
          aria-label={t('记忆来源', 'Memory source')}
          value={source}
          onChange={(e) => setSource(e.target.value)}
        >
          <option value="all">{t('所有来源', 'All sources')}</option>
          {[...new Set(state.notes.map((n) => n.source))].map((s) => (
            <option key={s}>{s}</option>
          ))}
        </select>
      </Toolbar>
      {notes.length > 0 && (
        <div className="memory-list">
          {notes.map((note) => (
            <button
              className={`memory-card ${note.scope}`}
              key={note.id}
              onClick={() => {
                setEditing({ ...note });
                setDeleting(false);
              }}
            >
              <span className={`memory-scope ${note.scope}`}>
                {note.scope === 'shared' ? <Globe2 size={13} /> : <Leaf size={13} />}
                {note.scope === 'shared' ? t('共享基础', 'Shared') : t('个人记忆', 'Personal')}
              </span>
              <span className="memory-text">
                <strong>{note.title}</strong>
                <span>{note.body}</span>
              </span>
              <span className="memory-meta">
                <span>{note.source}</span>
                <Time value={note.updatedAt} format="date" />
              </span>
              <Pencil size={14} className="row-chevron" />
            </button>
          ))}
        </div>
      )}
      {!notes.length && (
        <Empty
          icon={<Leaf size={24} />}
          title={t('还没有这样的记忆', 'Nothing here just yet')}
          detail={t(
            '换一个搜索词，或在对话里告诉助手值得记住的事。',
            'Try another search, or tell your assistant something worth remembering.',
          )}
        />
      )}
      <div className="page-note">
        <ShieldCheck size={14} />
        {t(
          '共享知识与个人偏好分别保存，不会自动合并不同人的记忆。',
          'Shared knowledge and personal preferences stay separate. Different people’s memories are never automatically merged.',
        )}
      </div>
      {editing && (
        <Modal title={t('编辑记忆', 'Edit memory')} close={() => setEditing(null)}>
          <div className="modal-body">
            <span className={`memory-scope ${editing.scope}`}>
              {editing.scope === 'shared' ? <Globe2 size={13} /> : <LockKeyhole size={13} />}
              {editing.scope === 'shared' ? t('共享基础', 'Shared foundation') : editing.source}
            </span>
            <label className="form-field">
              {t('标题', 'Title')}
              <input
                value={editing.title}
                onChange={(e) => setEditing({ ...editing, title: e.target.value })}
              />
            </label>
            <label className="form-field">
              {t('内容', 'Content')}
              <textarea
                rows={5}
                value={editing.body}
                onChange={(e) => setEditing({ ...editing, body: e.target.value })}
              />
            </label>
            <p className="field-help">
              {t('来源', 'Source')}：{editing.source}
            </p>
            {deleting && (
              <div className="error-note">
                {t(
                  '只删除这条记忆，聊天记录和其他资料会保留。再次点击确认删除。',
                  'Only this memory will be deleted. Conversations and other material are kept. Click again to confirm.',
                )}
              </div>
            )}
            <div className="modal-actions">
              <button
                className="button ghost danger"
                onClick={() => {
                  if (!deleting) {
                    setDeleting(true);
                    return;
                  }
                  dispatch({ type: 'note-delete', id: editing.id });
                  setEditing(null);
                  toast(t('记忆已删除', 'Memory deleted'));
                }}
              >
                <Trash2 size={14} />
                {deleting ? t('确认删除', 'Confirm deletion') : t('删除', 'Delete')}
              </button>
              <button
                className="button primary"
                disabled={!editing.title.trim() || !editing.body.trim()}
                onClick={() => {
                  dispatch({ type: 'note-save', note: { ...editing, updatedAt: Date.now() } });
                  setEditing(null);
                  toast(t('记忆已更新', 'Memory updated'));
                }}
              >
                {t('保存记忆', 'Save memory')}
              </button>
            </div>
          </div>
        </Modal>
      )}
    </PageFrame>
  );
}

export function SettingsPage({
  reset,
  exportSnapshot,
}: {
  reset: () => void;
  exportSnapshot: () => void;
}) {
  const { state, dispatch, t, toast } = useApp();
  const [confirmReset, setConfirmReset] = useState(false);
  const update = (value: Partial<typeof state.settings>) => dispatch({ type: 'settings', value });
  return (
    <PageFrame
      title={t('设置', 'Settings')}
      detail={t(
        '配置外观、模型和桌面对话的操作权限。',
        'Configure appearance, the model and local conversation permissions.',
      )}
    >
      <section className="settings-section">
        <h2>
          <SlidersHorizontal size={15} />
          {t('外观与语言', 'Appearance & language')}
        </h2>
        <SettingRow
          title={t('主题', 'Theme')}
          detail={t('选择浅色、深色或跟随系统。', 'Choose light, dark or your system preference.')}
        >
          <div className="segmented" role="group" aria-label={t('主题', 'Theme')}>
            {(
              [
                { id: 'light', icon: Sun, label: t('浅色', 'Light') },
                { id: 'dark', icon: Moon, label: t('深色', 'Dark') },
                { id: 'system', icon: Monitor, label: t('跟随系统', 'System') },
              ] as const
            ).map(({ id, icon: Icon, label }) => (
              <button
                key={id}
                data-testid={`theme-${id}`}
                aria-pressed={state.settings.theme === id}
                onClick={() => update({ theme: id })}
              >
                <Icon size={14} />
                {label}
              </button>
            ))}
          </div>
        </SettingRow>
        <SettingRow
          title={t('界面语言', 'Interface language')}
          detail={t(
            '只切换界面，不改写聊天内容。',
            'Changes the interface, not your conversations.',
          )}
        >
          <select
            aria-label={t('界面语言', 'Interface language')}
            data-testid="language-select"
            value={state.settings.language}
            onChange={(e) => update({ language: e.target.value as 'zh' | 'en' })}
          >
            <option value="zh">简体中文</option>
            <option value="en">English</option>
          </select>
        </SettingRow>
      </section>
      <section className="settings-section">
        <h2>
          <Settings2 size={15} />
          {t('助手偏好', 'Assistant preferences')}
        </h2>
        <SettingRow
          title={t('示例模型', 'Sample model')}
          detail={t(
            '用于展示模型选择，未连接真实服务。',
            'A model picker demonstration. No live service is connected.',
          )}
        >
          <select
            aria-label={t('示例模型', 'Sample model')}
            value={state.settings.model}
            onChange={(e) => update({ model: e.target.value as typeof state.settings.model })}
          >
            <option>Claude Sonnet</option>
            <option>GPT-5</option>
          </select>
        </SettingRow>
        <SettingRow
          title={t('应用内通知', 'In-app notifications')}
          detail={t(
            '任务有了结果，或有事情需要你关注时提醒。',
            'A heads-up when something is ready or needs attention.',
          )}
        >
          <Switch
            checked={state.settings.notifications}
            label={t('应用内通知', 'In-app notifications')}
            onChange={(notifications) => update({ notifications })}
          />
        </SettingRow>
        <SettingRow
          title={t('自动记忆', 'Auto memory')}
          detail={t(
            '积累有用的信息，关闭后保留已有记录。',
            'Keep useful context. Turning this off preserves existing notes.',
          )}
        >
          <Switch
            checked={state.settings.memory}
            label={t('自动记忆', 'Auto memory')}
            onChange={(memory) => update({ memory })}
          />
        </SettingRow>
      </section>
      <section className="settings-section">
        <h2>
          <ShieldCheck size={15} />
          {t('桌面对话权限', 'Desktop conversation permissions')}
        </h2>
        <SettingRow
          title={t('文件操作', 'File actions')}
          detail={t(
            '仅影响桌面自己的对话，不改变其他渠道。',
            'Applies to desktop conversations only. Channel rules stay separate.',
          )}
        >
          <select
            aria-label={t('文件操作权限', 'File action permission')}
            value={state.settings.permission}
            onChange={(e) =>
              update({ permission: e.target.value as typeof state.settings.permission })
            }
          >
            <option value="ask">{t('操作前询问', 'Ask before acting')}</option>
            <option value="allow">{t('允许演示操作', 'Allow demo actions')}</option>
            <option value="deny">{t('拒绝文件操作', 'Deny file actions')}</option>
          </select>
        </SettingRow>
      </section>
      <section className="settings-section">
        <h2>
          <FolderOpen size={15} />
          {t('工作区与演示数据', 'Workspace & demo data')}
        </h2>
        <SettingRow
          title={t('共享工作区', 'Shared workspace')}
          detail={t(
            '所有示例资料都在演示空间里，不读取本机文件。',
            'Sample materials live in the demo. Local files are not accessed.',
          )}
        >
          <code className="path-chip">workspace/</code>
        </SettingRow>
        <SettingRow
          title={t('本地数据', 'Local data')}
          detail={t(
            '存储在此浏览器中，关闭应用后执行停止。',
            'Saved in this browser. Execution stops when the app closes.',
          )}
        >
          <div className="setting-actions">
            <button className="button ghost" onClick={() => setConfirmReset(true)}>
              {t('重新开始演示', 'Restart demo')}
            </button>
            <button className="button secondary" onClick={exportSnapshot}>
              <ArrowDownToLine size={14} />
              {t('导出', 'Export')}
            </button>
          </div>
        </SettingRow>
      </section>
      <div className="settings-signature">
        <Logo />
        <span>
          Orangutan <span aria-hidden="true">·</span> {t('桌面原型', 'Desktop prototype')}
        </span>
      </div>
      {confirmReset && (
        <Modal
          title={t('重新开始演示？', 'Start the demo again?')}
          close={() => setConfirmReset(false)}
        >
          <div className="modal-body">
            <p>
              {t(
                '当前快照会先保留为浏览器中的备份，再加载初始示例。也可以先导出一份文件。',
                'Your current snapshot will be backed up in this browser before loading the initial examples. You can also export a copy first.',
              )}
            </p>
            <div className="modal-actions">
              <button className="button secondary" onClick={exportSnapshot}>
                {t('导出备份', 'Export backup')}
              </button>
              <button
                className="button primary"
                onClick={() => {
                  reset();
                  setConfirmReset(false);
                  toast(t('已请求重置演示', 'Demo reset requested'));
                }}
              >
                {t('确认重新开始', 'Start again')}
              </button>
            </div>
          </div>
        </Modal>
      )}
    </PageFrame>
  );
}
