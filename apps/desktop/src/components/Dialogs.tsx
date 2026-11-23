import { type KeyboardEvent as ReactKeyboardEvent } from 'react';
import {
  ArrowRight,
  Bell,
  CheckCheck,
  CircleAlert,
  CircleCheck,
  Clock3,
  ListChecks,
  MessageCircle,
  Search,
  Unplug,
} from 'lucide-react';
import { useApp } from '../context.ts';
import { Empty, Logo, Modal, PlatformIcon, Source, StatusBadge } from '../components.tsx';
import { PetAvatar } from '../pets.tsx';

const notificationIcons = {
  completed: CircleCheck,
  failed: CircleAlert,
  approval: Clock3,
  connection: Unplug,
};

export type ActiveDialog = 'search' | 'notifications' | 'about' | 'reset' | 'rename' | null;

export function AppDialogs({
  dialog,
  setDialog,
  query,
  setQuery,
  renaming,
  setRenaming,
  setTaskId,
  reset,
  exportSnapshot,
}: {
  dialog: ActiveDialog;
  setDialog: (next: ActiveDialog) => void;
  query: string;
  setQuery: (next: string) => void;
  renaming: { id: string; name: string } | null;
  setRenaming: (next: { id: string; name: string } | null) => void;
  setTaskId: (id: string | null) => void;
  reset: () => void;
  exportSnapshot: () => void;
}) {
  const { state, runtime, dispatch, t, navigate, selectConversation } = useApp();
  const connectionName = (connectionId: string) =>
    state.connections.find((c) => c.id === connectionId)?.name ?? t('桌面', 'Desktop');
  const needle = query.trim().toLocaleLowerCase();
  const matchingConversations = state.conversations.filter((c) =>
    `${c.name} ${connectionName(c.connectionId)}`.toLocaleLowerCase().includes(needle),
  );
  const matchingTasks = state.tasks.filter((task) =>
    task.title.toLocaleLowerCase().includes(needle),
  );
  const movePaletteFocus = (event: ReactKeyboardEvent<HTMLElement>) => {
    if (event.key !== 'ArrowDown' && event.key !== 'ArrowUp') return;
    const items = Array.from(
      event.currentTarget.querySelectorAll<HTMLElement>('input, .palette-results button'),
    );
    const index = items.indexOf(document.activeElement as HTMLElement);
    const step = event.key === 'ArrowDown' ? 1 : -1;
    event.preventDefault();
    items[(index + step + items.length) % items.length]?.focus();
  };

  if (dialog === 'search') {
    return (
      <Modal
        title={t('搜索对话和任务', 'Search conversations and tasks')}
        close={() => setDialog(null)}
        variant="palette"
      >
        <div className="palette" onKeyDown={movePaletteFocus}>
          <label className="palette-input">
            <Search size={18} />
            <input
              aria-label={t('搜索关键词', 'Search query')}
              value={query}
              placeholder={t('搜索对话、渠道或任务…', 'Search conversations, channels or tasks…')}
              onChange={(e) => setQuery(e.target.value)}
              onKeyDown={(e) => {
                if (e.key !== 'Enter' || e.nativeEvent.isComposing) return;
                e.preventDefault();
                e.currentTarget
                  .closest('.palette')
                  ?.querySelector<HTMLButtonElement>('.palette-results button')
                  ?.click();
              }}
            />
            <kbd>Esc</kbd>
          </label>
          <div className="palette-results">
            {matchingConversations.length > 0 && <h3>{t('对话', 'Conversations')}</h3>}
            {matchingConversations.map((c) => {
              const connection = state.connections.find((v) => v.id === c.connectionId);
              return (
                <button
                  key={c.id}
                  onClick={() => {
                    selectConversation(c.id);
                    setDialog(null);
                  }}
                >
                  <span className="palette-icon">
                    {connection ? (
                      <PlatformIcon platform={connection.platform} size={15} />
                    ) : (
                      <MessageCircle size={15} />
                    )}
                  </span>
                  <span className="palette-text">
                    {c.name}
                    <small>{connectionName(c.connectionId)}</small>
                  </span>
                  <ArrowRight size={14} className="palette-arrow" />
                </button>
              );
            })}
            {matchingTasks.length > 0 && <h3>{t('任务', 'Tasks')}</h3>}
            {matchingTasks.map((task) => (
              <button
                key={task.id}
                onClick={() => {
                  selectConversation(task.conversationId);
                  setTaskId(task.id);
                  setDialog(null);
                }}
              >
                <span className="palette-icon">
                  <ListChecks size={15} />
                </span>
                <span className="palette-text">
                  {task.title}
                  <Source conversationId={task.conversationId} />
                </span>
                <StatusBadge status={task.status} />
              </button>
            ))}
            {!matchingConversations.length && !matchingTasks.length && (
              <Empty
                icon={<Search size={22} />}
                title={t('没有找到', 'No matches yet')}
                detail={t('换个关键词再试试。', 'Try another word.')}
              />
            )}
          </div>
          <div className="palette-footer" aria-hidden="true">
            <span>
              <kbd>↑</kbd>
              <kbd>↓</kbd>
              {t('选择', 'Move')}
            </span>
            <span>
              <kbd>Enter</kbd>
              {t('打开', 'Open')}
            </span>
            <span>
              <kbd>Esc</kbd>
              {t('关闭', 'Close')}
            </span>
          </div>
        </div>
      </Modal>
    );
  }

  if (dialog === 'notifications') {
    return (
      <Modal title={t('通知', 'Notifications')} close={() => setDialog(null)}>
        <div className="notification-list">
          {state.notifications.map((n) => {
            const Icon = notificationIcons[n.kind];
            return (
              <div className={`notification-item ${n.read ? 'read' : ''}`} key={n.id}>
                <span className={`notification-icon ${n.kind}`}>
                  <Icon size={15} />
                </span>
                <button
                  className="notification-content"
                  onClick={() => {
                    dispatch({ type: 'read-notification', id: n.id });
                    if (n.conversationId) selectConversation(n.conversationId);
                    else navigate('connections');
                    if (n.taskId) setTaskId(n.taskId);
                    setDialog(null);
                  }}
                >
                  <strong>{n.title}</strong>
                  <span className="notification-detail">{n.detail}</span>
                  {n.conversationId && <Source conversationId={n.conversationId} />}
                </button>
                {!n.read && (
                  <button
                    className="icon-button small"
                    aria-label={t('标记已读', 'Mark as read')}
                    title={t('标记已读', 'Mark as read')}
                    onClick={() => dispatch({ type: 'read-notification', id: n.id })}
                  >
                    <CheckCheck size={15} />
                  </button>
                )}
              </div>
            );
          })}
          {!state.notifications.length && (
            <Empty
              icon={<Bell size={22} />}
              title={t('暂时没有新消息', 'All caught up')}
              detail={t('有新的进展时，会出现在这里。', 'New updates will find their way here.')}
            />
          )}
        </div>
      </Modal>
    );
  }

  if (dialog === 'rename' && renaming) {
    return (
      <Modal title={t('重命名对话', 'Rename conversation')} close={() => setDialog(null)}>
        <form
          className="modal-body"
          onSubmit={(event) => {
            event.preventDefault();
            if (!renaming.name.trim() || renaming.name.trim().length > 80) return;
            dispatch({ type: 'rename', conversationId: renaming.id, name: renaming.name });
            setDialog(null);
          }}
        >
          <label className="form-field">
            {t('对话名称', 'Conversation name')}
            <input
              value={renaming.name}
              maxLength={80}
              onFocus={(event) => event.currentTarget.select()}
              onChange={(event) => setRenaming({ ...renaming, name: event.target.value })}
            />
          </label>
          <div className="modal-actions">
            <button type="button" className="button secondary" onClick={() => setDialog(null)}>
              {t('取消', 'Cancel')}
            </button>
            <button className="button primary" disabled={!renaming.name.trim()}>
              {t('保存名称', 'Save name')}
            </button>
          </div>
        </form>
      </Modal>
    );
  }

  if (dialog === 'about') {
    return (
      <Modal title={t('关于 Orangutan', 'About Orangutan')} close={() => setDialog(null)}>
        <div className="modal-body">
          <div className="about-brand">
            {state.settings.companions !== false ? <PetAvatar pet="oran" size={40} /> : <Logo />}
            <span>
              <strong>Orangutan</strong>
              <small>
                {runtime.mode === 'offline'
                  ? t('桌面工作台 · 离线演示模式', 'Desktop workbench · Offline demo')
                  : t(
                      'Personal Agent · 已连接 C++26 宿主',
                      'Personal Agent · Connected to C++26 host',
                    )}
              </small>
            </span>
          </div>
          <p>
            {runtime.mode === 'offline'
              ? t(
                  '当前未检测到本地 `orangutan serve`，回复与渠道连接由浏览器演示数据驱动。运行 `orangutan serve --demo` 或带模型配置启动后刷新页面，即可自动接入真实 C++26 运行时与 SQLite 存储。',
                  'No local `orangutan serve` detected, so replies and channel events use browser demo data. Start `orangutan serve --demo` (or with a provider config) and refresh to connect to the live C++26 runtime.',
                )
              : t(
                  '当前已直接连接本地 C++26 Personal Agent 宿主，对话、权限审批、SQLite 长期记忆与提醒均由真实运行时驱动。',
                  'Connected directly to your local C++26 Personal Agent host. Conversations, approvals, SQLite memory and reminders run live.',
                )}
          </p>
          <div className="modal-actions">
            <button className="button primary" onClick={() => setDialog(null)}>
              {t('知道了', 'Got it')}
            </button>
          </div>
        </div>
      </Modal>
    );
  }

  if (dialog === 'reset') {
    return (
      <Modal title={t('恢复演示数据', 'Recover demo data')} close={() => setDialog(null)}>
        <div className="modal-body">
          <p>
            {t(
              '已有快照会先保留为本地备份，再加载初始示例。存储不可用时不会强行覆盖。',
              'The existing snapshot is backed up before initial examples are loaded. Unavailable storage will not be overwritten.',
            )}
          </p>
          <div className="modal-actions">
            <button className="button secondary" onClick={exportSnapshot}>
              {t('导出快照', 'Export snapshot')}
            </button>
            <button
              className="button primary"
              onClick={() => {
                reset();
                setDialog(null);
                setTaskId(null);
              }}
            >
              {t('确认恢复', 'Confirm recovery')}
            </button>
          </div>
        </div>
      </Modal>
    );
  }

  return null;
}
