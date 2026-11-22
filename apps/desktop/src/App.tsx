import { useEffect, useRef, useState, type KeyboardEvent as ReactKeyboardEvent } from 'react';
import {
  ArrowRight,
  Bell,
  Check,
  CheckCheck,
  CircleAlert,
  CircleCheck,
  CircleHelp,
  Clock3,
  Leaf,
  ListChecks,
  Menu,
  MessageCircle,
  Pencil,
  Radio,
  Search,
  Settings2,
  SquarePen,
  Unplug,
} from 'lucide-react';
import { AppContext, type View } from './context.ts';
import { useAgent } from './useAgent.ts';
import { Assistant } from './Assistant.tsx';
import { ConnectionsPage, MemoryPage, SettingsPage, TasksPage } from './Pages.tsx';
import { TaskPanel } from './TaskPanel.tsx';
import { Empty, Logo, Modal, PlatformIcon, Source, StatusBadge } from './components.tsx';
import { AppShell, Sidebar, SidebarSection, Topbar } from './layout.tsx';

const notificationIcons = {
  completed: CircleCheck,
  failed: CircleAlert,
  approval: Clock3,
  connection: Unplug,
};

export default function App() {
  const { state, dispatch, storageError, reset, exportSnapshot } = useAgent();
  const [view, setView] = useState<View>('assistant');
  const [taskId, setTaskId] = useState<string | null>(null);
  const [sidebarOpen, setSidebarOpen] = useState(false);
  const sidebar = useRef<HTMLElement>(null);
  const menu = useRef<HTMLButtonElement>(null);
  const [dialog, setDialog] = useState<
    'search' | 'notifications' | 'about' | 'reset' | 'rename' | null
  >(null);
  const [query, setQuery] = useState('');
  const [renaming, setRenaming] = useState<{ id: string; name: string } | null>(null);
  const [toast, setToast] = useState('');
  const t = (zh: string, en: string) => (state.settings.language === 'zh' ? zh : en);
  const navigate = (next: View) => {
    setView(next);
    setTaskId(null);
    setSidebarOpen(false);
  };
  const selectConversation = (id: string) => {
    dispatch({ type: 'select', id });
    setView('assistant');
    setTaskId(null);
    setSidebarOpen(false);
  };
  const newConversation = () => {
    dispatch({ type: 'new', id: crypto.randomUUID(), now: Date.now() });
    navigate('assistant');
  };
  const selected = state.conversations.find((c) => c.id === state.selectedId)!;
  const selectedConnection = state.connections.find((c) => c.id === selected.connectionId);
  const unread = state.notifications.filter((n) => !n.read).length;
  const activeTasks = state.tasks.filter((task) =>
    ['queued', 'running', 'awaiting_approval'].includes(task.status),
  ).length;
  const navItems = [
    { id: 'assistant' as const, icon: MessageCircle, label: t('助手', 'Assistant') },
    { id: 'tasks' as const, icon: ListChecks, label: t('任务', 'Tasks') },
    { id: 'connections' as const, icon: Radio, label: t('连接', 'Connections') },
    { id: 'memory' as const, icon: Leaf, label: t('记忆', 'Memory') },
    { id: 'settings' as const, icon: Settings2, label: t('设置', 'Settings') },
  ];
  const connectionName = (connectionId: string) =>
    state.connections.find((c) => c.id === connectionId)?.name ?? t('桌面', 'Desktop');
  useEffect(() => {
    const media = window.matchMedia('(prefers-color-scheme: dark)');
    const update = () => {
      const next =
        state.settings.theme === 'dark' || (state.settings.theme === 'system' && media.matches);
      document.documentElement.dataset.theme = next ? 'dark' : 'light';
      document
        .querySelector('meta[name="theme-color"]')
        ?.setAttribute('content', next ? '#211c16' : '#f6f2ea');
    };
    update();
    media.addEventListener('change', update);
    return () => media.removeEventListener('change', update);
  }, [state.settings.theme]);
  useEffect(() => {
    document.documentElement.lang = state.settings.language === 'zh' ? 'zh-CN' : 'en';
    document.title = t('Orangutan · 你的本地助手', 'Orangutan · Your local assistant');
  }, [state.settings.language]);
  useEffect(() => {
    const handler = (event: KeyboardEvent) => {
      if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'k') {
        event.preventDefault();
        setSidebarOpen(false);
        setQuery('');
        setDialog((previous) => (previous === 'search' ? null : 'search'));
      }
      if (event.key === 'Escape') setSidebarOpen(false);
    };
    window.addEventListener('keydown', handler);
    return () => window.removeEventListener('keydown', handler);
  }, []);
  useEffect(() => {
    if (!toast) return;
    const timer = setTimeout(() => setToast(''), 3000);
    return () => clearTimeout(timer);
  }, [toast]);
  useEffect(() => {
    const media = window.matchMedia('(min-width: 781px)');
    const closeWide = () => {
      if (media.matches) setSidebarOpen(false);
    };
    media.addEventListener('change', closeWide);
    return () => media.removeEventListener('change', closeWide);
  }, []);
  useEffect(() => {
    if (!sidebarOpen) return;
    const buttons = () =>
      Array.from(
        sidebar.current?.querySelectorAll<HTMLButtonElement>('button:not(:disabled)') ?? [],
      );
    let disposed = false;
    const frame = requestAnimationFrame(() => {
      const transitions = sidebar.current?.getAnimations() ?? [];
      void Promise.allSettled(transitions.map((animation) => animation.finished)).then(() => {
        if (!disposed) buttons()[0]?.focus();
      });
    });
    const trap = (event: KeyboardEvent) => {
      if (event.key !== 'Tab') return;
      const items = buttons();
      const first = items[0];
      const last = items.at(-1);
      if (event.shiftKey && document.activeElement === first) {
        event.preventDefault();
        last?.focus();
      } else if (!event.shiftKey && document.activeElement === last) {
        event.preventDefault();
        first?.focus();
      }
    };
    document.addEventListener('keydown', trap);
    return () => {
      disposed = true;
      cancelAnimationFrame(frame);
      document.removeEventListener('keydown', trap);
      if (!document.querySelector('dialog[open]') && menu.current?.getClientRects().length)
        menu.current.focus();
    };
  }, [sidebarOpen]);
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
  const conversationLink = (id: string) =>
    view === 'assistant' && state.selectedId === id ? 'true' : undefined;

  return (
    <AppContext.Provider
      value={{
        state,
        dispatch,
        t,
        view,
        navigate,
        selectConversation,
        openTask: setTaskId,
        toast: setToast,
      }}
    >
      <AppShell
        hasPanel={Boolean(taskId)}
        sidebarOpen={sidebarOpen}
        onCloseSidebar={() => setSidebarOpen(false)}
        closeLabel={t('关闭导航', 'Close navigation')}
        sidebar={
          <Sidebar label={t('主导航', 'Main navigation')} innerRef={sidebar}>
            <div className="sidebar-head">
              <button
                className="brand"
                onClick={() => navigate('assistant')}
                aria-label={t('Orangutan 首页', 'Orangutan home')}
              >
                <Logo />
                <strong>Orangutan</strong>
              </button>
            </div>
            <button className="new-conversation" onClick={newConversation}>
              <SquarePen size={15} />
              <span>{t('新对话', 'New conversation')}</span>
            </button>
            <nav className="nav-list">
              {navItems.slice(0, 2).map(({ id, icon: Icon, label }) => (
                <button
                  key={id}
                  data-testid={`nav-${id}`}
                  className="nav-item"
                  aria-current={view === id ? 'page' : undefined}
                  onClick={() => navigate(id)}
                >
                  <Icon size={16} strokeWidth={1.75} />
                  <span>{label}</span>
                  {id === 'tasks' && activeTasks > 0 && (
                    <>
                      <span className="nav-dot" aria-hidden="true" />
                      <span className="visually-hidden">
                        {t(`${activeTasks} 项进行中`, `${activeTasks} active`)}
                      </span>
                    </>
                  )}
                </button>
              ))}
            </nav>
            <div className="sidebar-history">
              <SidebarSection label={t('对话', 'Chats')}>
                {state.conversations
                  .filter((c) => c.origin === 'desktop')
                  .map((c) => (
                    <button
                      className="conversation-link"
                      aria-current={conversationLink(c.id)}
                      key={c.id}
                      onClick={() => selectConversation(c.id)}
                    >
                      <MessageCircle size={14} />
                      <span className="conversation-text">{c.name}</span>
                    </button>
                  ))}
              </SidebarSection>
              <SidebarSection label={t('渠道', 'Channels')}>
                {state.conversations
                  .filter((c) => c.origin === 'channel')
                  .map((c) => {
                    const connection = state.connections.find((v) => v.id === c.connectionId)!;
                    return (
                      <button
                        className="conversation-link"
                        aria-current={conversationLink(c.id)}
                        key={c.id}
                        data-testid={`conversation-${connection.id}`}
                        onClick={() => selectConversation(c.id)}
                      >
                        <PlatformIcon platform={connection.platform} size={14} />
                        <span className="conversation-text">{c.name}</span>
                        <span className="visually-hidden">{connection.name}</span>
                        {c.unread > 0 && <span className="unread-dot" />}
                      </button>
                    );
                  })}
              </SidebarSection>
            </div>
            <div className="sidebar-bottom">
              <nav className="nav-list" aria-label={t('工作区管理', 'Workspace management')}>
                {navItems.slice(2).map(({ id, icon: Icon, label }) => (
                  <button
                    key={id}
                    data-testid={`nav-${id}`}
                    className="nav-item"
                    aria-current={view === id ? 'page' : undefined}
                    onClick={() => navigate(id)}
                  >
                    <Icon size={16} strokeWidth={1.75} />
                    <span>{label}</span>
                  </button>
                ))}
              </nav>
              <div className="sidebar-meta">
                <span className="demo-state">
                  <span aria-hidden="true" />
                  {t('演示模式', 'Demo mode')}
                </span>
                <button
                  className="icon-button small"
                  aria-label={t('关于演示', 'About this demo')}
                  onClick={() => setDialog('about')}
                >
                  <CircleHelp size={15} />
                </button>
              </div>
            </div>
          </Sidebar>
        }
      >
        <Topbar>
          <button
            ref={menu}
            className="icon-button menu-button"
            aria-label={t('打开导航', 'Open navigation')}
            aria-expanded={sidebarOpen}
            onClick={() => setSidebarOpen(!sidebarOpen)}
          >
            <Menu size={18} />
          </button>
          {view === 'assistant' && (
            <div className="conversation-heading">
              <h1>{selected.name}</h1>
              {selected.origin === 'channel' && selectedConnection && (
                <span className="conversation-source">
                  <PlatformIcon platform={selectedConnection.platform} size={13} />
                  {selectedConnection.name}
                </span>
              )}
              {selected.origin === 'desktop' && (
                <button
                  className="icon-button small"
                  aria-label={t('重命名对话', 'Rename conversation')}
                  title={t('重命名对话', 'Rename conversation')}
                  onClick={() => {
                    setRenaming({ id: selected.id, name: selected.name });
                    setDialog('rename');
                  }}
                >
                  <Pencil size={13} />
                </button>
              )}
            </div>
          )}
          <div className="topbar-actions">
            <button
              className="icon-button"
              onClick={() => {
                setQuery('');
                setDialog('search');
              }}
              aria-label={t('搜索全部内容', 'Search everything')}
              aria-keyshortcuts="Control+K Meta+K"
              title={`${t('搜索', 'Search')} · Ctrl K`}
            >
              <Search size={17} />
            </button>
            <button
              className="icon-button notification-button"
              aria-label={t('通知', 'Notifications')}
              onClick={() => setDialog('notifications')}
            >
              <Bell size={17} />
              {unread > 0 && <span className="notification-count">{unread}</span>}
            </button>
          </div>
        </Topbar>
        {storageError && (
          <div className="storage-banner" role="alert">
            <CircleAlert size={15} />
            <span>
              {t(
                '本地快照暂时无法读取或保存。已有数据不会被自动清除。',
                'The local snapshot could not be read or saved. Existing data will not be automatically cleared.',
              )}
            </span>
            <button className="text-button" onClick={exportSnapshot}>
              {t('导出', 'Export')}
            </button>
            <button className="text-button" onClick={() => setDialog('reset')}>
              {t('恢复演示', 'Recover demo')}
            </button>
          </div>
        )}
        <div className="workspace-body">
          <main key={view} className="main-content" id="main-content" tabIndex={-1}>
            {view === 'assistant' ? (
              <Assistant />
            ) : view === 'tasks' ? (
              <TasksPage />
            ) : view === 'connections' ? (
              <ConnectionsPage />
            ) : view === 'memory' ? (
              <MemoryPage />
            ) : (
              <SettingsPage reset={reset} exportSnapshot={exportSnapshot} />
            )}
          </main>
          {taskId && <TaskPanel key={taskId} id={taskId} close={() => setTaskId(null)} />}
        </div>
      </AppShell>
      {toast && (
        <div className="toast" role="status">
          <Check size={15} />
          {toast}
        </div>
      )}
      {dialog === 'search' && (
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
      )}
      {dialog === 'notifications' && (
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
      )}
      {dialog === 'rename' && renaming && (
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
      )}
      {dialog === 'about' && (
        <Modal title={t('关于 Orangutan', 'About Orangutan')} close={() => setDialog(null)}>
          <div className="modal-body">
            <div className="about-brand">
              <Logo />
              <span>
                <strong>Orangutan</strong>
                <small>{t('桌面原型 · 演示模式', 'Desktop prototype · Demo mode')}</small>
              </span>
            </div>
            <p>
              {t(
                '这是一个可交互的桌面界面原型。回复、小队工作和渠道连接均使用演示数据；不会发送真实消息、调用模型或修改本机文件。',
                'This is an interactive desktop interface prototype. Replies, teamwork and channel connections use demo data. It sends no real messages, calls no model and changes no local files.',
              )}
            </p>
            <p>
              {t(
                '历史保存在当前浏览器中。关闭或刷新页面后，未完成工作会标为中断。',
                'History is saved in this browser. Unfinished work is marked interrupted after closing or refreshing the page.',
              )}
            </p>
            <div className="modal-actions">
              <button className="button primary" onClick={() => setDialog(null)}>
                {t('知道了', 'Got it')}
              </button>
            </div>
          </div>
        </Modal>
      )}
      {dialog === 'reset' && (
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
      )}
    </AppContext.Provider>
  );
}
