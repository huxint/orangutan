import { useEffect, useRef, useState } from 'react';
import {
  Bell,
  BellRing,
  Check,
  CircleAlert,
  CircleHelp,
  Leaf,
  ListChecks,
  Menu,
  MessageCircle,
  Pencil,
  Radio,
  Search,
  Settings2,
  SquarePen,
} from 'lucide-react';
import { AppContext, type View } from './context.ts';
import { useAgent } from './useAgent.ts';
import { Assistant } from './Assistant.tsx';
import { TasksPage } from './pages/TasksPage.tsx';
import { RemindersPage } from './pages/RemindersPage.tsx';
import { ConnectionsPage } from './pages/ConnectionsPage.tsx';
import { MemoryPage } from './pages/MemoryPage.tsx';
import { SettingsPage } from './pages/SettingsPage.tsx';
import { TaskPanel } from './TaskPanel.tsx';
import { AppDialogs, type ActiveDialog } from './components/Dialogs.tsx';
import { Logo, PlatformIcon } from './components.tsx';
import { AppShell, Sidebar, SidebarSection, Topbar } from './layout.tsx';
import { deriveCompanion, PetAvatar } from './pets.tsx';

export default function App() {
  const { state, runtime, dispatch, storageError, reset, exportSnapshot } = useAgent();
  const [view, setView] = useState<View>('assistant');
  const [taskId, setTaskId] = useState<string | null>(null);
  const [sidebarOpen, setSidebarOpen] = useState(false);
  const sidebar = useRef<HTMLElement>(null);
  const menu = useRef<HTMLButtonElement>(null);
  const [dialog, setDialog] = useState<ActiveDialog>(null);
  const [query, setQuery] = useState('');
  const [renaming, setRenaming] = useState<{ id: string; name: string } | null>(null);
  const [toast, setToast] = useState('');
  const t = (zh: string, en: string) => (state.settings.language === 'zh' ? zh : en);
  const companions = state.settings.companions !== false;
  const companion = deriveCompanion(state, runtime.activeCompanion);

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
  const openReminders = (state.reminders ?? []).filter((r) => !r.completed).length;

  const navItems = [
    { id: 'assistant' as const, icon: MessageCircle, label: t('助手', 'Assistant') },
    { id: 'tasks' as const, icon: ListChecks, label: t('任务', 'Tasks') },
    { id: 'reminders' as const, icon: BellRing, label: t('提醒', 'Reminders') },
    { id: 'connections' as const, icon: Radio, label: t('连接', 'Connections') },
    { id: 'memory' as const, icon: Leaf, label: t('记忆', 'Memory') },
    { id: 'settings' as const, icon: Settings2, label: t('设置', 'Settings') },
  ];

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
    document.title = t('Orangutan · 你的个人助手', 'Orangutan · Your personal agent');
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

  const conversationLink = (id: string) =>
    view === 'assistant' && state.selectedId === id ? 'true' : undefined;

  return (
    <AppContext.Provider
      value={{
        state,
        runtime,
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
              {navItems.slice(0, 3).map(({ id, icon: Icon, label }) => (
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
                  {id === 'reminders' && openReminders > 0 && (
                    <span className="nav-dot" aria-hidden="true" />
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
                {navItems.slice(3).map(({ id, icon: Icon, label }) => (
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
              {companions && (
                <div className="companion-pill" title={t(companion.captionZh, companion.captionEn)}>
                  <PetAvatar pet={companion.pet} mood={companion.mood} size={28} />
                  <span className="companion-pill-text">
                    <strong>{t(companion.nameZh, companion.nameEn)}</strong>
                    <small>{t(companion.captionZh, companion.captionEn)}</small>
                  </span>
                </div>
              )}
              <div className="sidebar-meta">
                <span className={`demo-state${runtime.mode !== 'offline' ? ' is-live' : ''}`}>
                  <span aria-hidden="true" />
                  {runtime.mode === 'offline'
                    ? t('演示模式', 'Demo mode')
                    : t('已连本地运行时', 'Local runtime')}
                </span>
                <button
                  className="icon-button small"
                  aria-label={t('关于 Orangutan', 'About Orangutan')}
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
            ) : view === 'reminders' ? (
              <RemindersPage />
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
      <AppDialogs
        dialog={dialog}
        setDialog={setDialog}
        query={query}
        setQuery={setQuery}
        renaming={renaming}
        setRenaming={setRenaming}
        setTaskId={setTaskId}
        reset={reset}
        exportSnapshot={exportSnapshot}
      />
    </AppContext.Provider>
  );
}
