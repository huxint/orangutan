import { Fragment, useEffect, useRef, useState, type RefObject } from 'react';
import {
  ArrowDown,
  ArrowRight,
  ArrowUp,
  FileText,
  LockKeyhole,
  Paperclip,
  Search,
  Square,
  Users,
} from 'lucide-react';
import { useApp } from './context.ts';
import { active } from './model.ts';
import { Approval, Logo, Markdown, TaskCard, Time } from './components.tsx';

function Composer({ inputRef }: { inputRef: RefObject<HTMLTextAreaElement | null> }) {
  const { state, dispatch, t, toast } = useApp();
  const conversationId = state.selectedId;
  const draft = state.drafts[conversationId] ?? '';
  const running = state.tasks.find(
    (task) => task.conversationId === conversationId && active(task.status),
  );
  const send = () => {
    if (draft.trim() && !running)
      dispatch({
        type: 'send',
        conversationId,
        text: draft,
        id: crypto.randomUUID(),
        now: Date.now(),
      });
  };
  return (
    <div className="composer-wrap">
      <div className="composer" data-testid="composer">
        <textarea
          ref={inputRef}
          aria-label={t('给助手发消息', 'Message your assistant')}
          placeholder={t('描述任务，或输入问题…', 'Describe a task or ask a question…')}
          value={draft}
          rows={1}
          autoFocus
          title={t(
            'Enter 发送 · Shift + Enter 换行',
            'Enter to send · Shift + Enter for a new line',
          )}
          onChange={(e) => dispatch({ type: 'draft', conversationId, text: e.target.value })}
          onKeyDown={(e) => {
            if (e.key === 'Enter' && !e.shiftKey && !e.nativeEvent.isComposing) {
              e.preventDefault();
              send();
            }
          }}
        />
        <div className="composer-tools">
          <button
            className="icon-button"
            aria-label={t('添加示例资料', 'Attach sample notes')}
            title={t('添加示例资料', 'Attach sample notes')}
            onClick={() => {
              dispatch({
                type: 'draft',
                conversationId,
                text:
                  draft +
                  (draft ? '\n' : '') +
                  t(
                    '请参考示例文件 meeting-notes.md，整理会议笔记。',
                    'Organize the sample meeting-notes.md file.',
                  ),
              });
              inputRef.current?.focus();
              toast(t('已添加示例资料', 'Sample notes attached'));
            }}
          >
            <Paperclip size={16} />
          </button>
          <span className="composer-spacer" />
          {running ? (
            <button
              className="send-button stop"
              aria-label={t('停止本地任务', 'Stop local task')}
              disabled={running.status === 'cancelling'}
              onClick={() => dispatch({ type: 'stop', taskId: running.id, now: Date.now() })}
            >
              <Square size={12} fill="currentColor" />
            </button>
          ) : (
            <button
              className="send-button"
              aria-label={t('发送消息', 'Send message')}
              disabled={!draft.trim()}
              onClick={send}
            >
              <ArrowUp size={17} />
            </button>
          )}
        </div>
      </div>
    </div>
  );
}

const dayOf = (time: number) => new Date(time).toDateString();

export function Assistant() {
  const { state, dispatch, t, navigate } = useApp();
  const conversation = state.conversations.find((c) => c.id === state.selectedId)!;
  const local = conversation.origin === 'desktop';
  const messages = state.messages.filter((m) => m.conversationId === conversation.id);
  const tasks = state.tasks.filter((task) => task.conversationId === conversation.id);
  const running = tasks.find((task) => active(task.status));
  const scroll = useRef<HTMLDivElement>(null);
  const textarea = useRef<HTMLTextAreaElement>(null);
  const pinned = useRef(true);
  const jumping = useRef(false);
  const [atBottom, setAtBottom] = useState(true);
  const lastText = messages.at(-1)?.text;
  useEffect(() => {
    pinned.current = true;
    jumping.current = false;
    setAtBottom(true);
    const element = scroll.current;
    const content = element?.firstElementChild;
    if (!element || !content) return;
    element.scrollTop = element.scrollHeight;
    const observer = new ResizeObserver(() => {
      if (pinned.current && !jumping.current) element.scrollTop = element.scrollHeight;
    });
    observer.observe(content);
    return () => observer.disconnect();
  }, [conversation.id, messages.length === 0]);
  useEffect(() => {
    if (scroll.current && pinned.current && !jumping.current)
      scroll.current.scrollTop = scroll.current.scrollHeight;
  }, [lastText, running?.status]);
  const cancelJump = () => {
    if (!jumping.current) return;
    jumping.current = false;
    pinned.current = false;
    setAtBottom(false);
  };
  const suggest = (text: string) => {
    dispatch({ type: 'draft', conversationId: conversation.id, text });
    textarea.current?.focus();
  };
  const prompts = [
    {
      icon: FileText,
      title: t('整理资料', 'Organize files'),
      prompt: t(
        '汇总工作区中的会议记录，列出决策、负责人和待办事项。',
        'Summarize the workspace meeting notes, including decisions, owners and action items.',
      ),
    },
    {
      icon: Users,
      title: t('拆解计划', 'Plan a task'),
      prompt: t(
        '组织小队，制定下周的工作计划，对比优先级和时间安排。',
        'Plan next week’s work with a team. Compare priorities and scheduling options.',
      ),
    },
    {
      icon: Search,
      title: t('分析问题', 'Analyze a question'),
      prompt: t(
        '帮我分析这个问题：如何减少日常工作中的重复整理？',
        'Help me analyze ways to reduce repetitive organization in my daily work.',
      ),
    },
  ];
  const activeCount = state.tasks.filter((task) => active(task.status)).length;
  const empty = messages.length === 0 && local;
  const threadTask = (task: (typeof tasks)[number]) => (
    <div key={task.id} className={`thread-task${active(task.status) ? ' active' : ''}`}>
      <TaskCard task={task} />
      <Approval task={task} />
    </div>
  );
  return (
    <div className={'assistant-view' + (empty ? ' is-empty' : '')}>
      {empty ? (
        <div className="start-workspace">
          <div className="start-inner">
            <section
              className="start-session"
              aria-label={t('开始本地会话', 'Start a local session')}
            >
              <h1>{t('今天想完成什么？', 'What should we get done?')}</h1>
              <Composer inputRef={textarea} />
              <div className="request-shortcuts">
                {prompts.map(({ icon: Icon, title, prompt }) => (
                  <button key={title} onClick={() => suggest(prompt)}>
                    <Icon size={15} />
                    <span>{title}</span>
                  </button>
                ))}
              </div>
              {activeCount > 0 && (
                <button className="active-work" onClick={() => navigate('tasks')}>
                  <span className="active-dot" aria-hidden="true" />
                  {t(
                    `${activeCount} 项工作正在进行`,
                    `${activeCount} ${activeCount === 1 ? 'task' : 'tasks'} in progress`,
                  )}
                  <ArrowRight size={13} />
                </button>
              )}
            </section>
          </div>
        </div>
      ) : (
        <>
          <div
            className="conversation-scroll"
            ref={scroll}
            tabIndex={-1}
            aria-label={t('会话记录', 'Conversation history')}
            onWheel={cancelJump}
            onTouchStart={cancelJump}
            onPointerDown={cancelJump}
            onKeyDown={(event) => {
              if (
                ['ArrowUp', 'ArrowDown', 'PageUp', 'PageDown', 'Home', 'End', ' '].includes(
                  event.key,
                )
              )
                cancelJump();
            }}
            onScrollEnd={() => {
              if (!jumping.current || !scroll.current) return;
              jumping.current = false;
              scroll.current.scrollTop = scroll.current.scrollHeight;
              pinned.current = true;
              setAtBottom(true);
            }}
            onScroll={() => {
              const element = scroll.current;
              if (!element || jumping.current) return;
              pinned.current = element.scrollHeight - element.scrollTop - element.clientHeight < 64;
              setAtBottom(pinned.current);
            }}
          >
            <div className="thread">
              {messages.map((message, index) => (
                <Fragment key={message.id}>
                  {(index === 0 ||
                    dayOf(messages[index - 1].createdAt) !== dayOf(message.createdAt)) && (
                    <div className="thread-date">
                      <Time value={message.createdAt} format="day" />
                    </div>
                  )}
                  <div className={`message ${message.role} ${message.state}`}>
                    <div className="message-body">
                      {message.role === 'assistant' && (
                        <div className="message-author">
                          <span className="message-mark">
                            <Logo />
                          </span>
                          <strong>Orangutan</strong>
                        </div>
                      )}
                      <Markdown>{message.text}</Markdown>
                      {message.state === 'interrupted' && (
                        <small className="message-note">
                          {t('回复已中断', 'Response interrupted')}
                        </small>
                      )}
                    </div>
                  </div>
                  {tasks.filter((task) => message.id === `${task.id}:user`).map(threadTask)}
                </Fragment>
              ))}
              {tasks
                .filter((task) => !messages.some((message) => message.id === `${task.id}:user`))
                .map(threadTask)}
              {running && running.status !== 'awaiting_approval' && (
                <div className="thinking">
                  {running.status === 'running' && (
                    <span className="thinking-dots" aria-hidden="true">
                      <i />
                      <i />
                      <i />
                    </span>
                  )}
                  {running.status === 'cancelling'
                    ? t('正在取消执行', 'Cancelling execution')
                    : t('正在执行', 'Executing')}
                </div>
              )}
            </div>
          </div>
          <div className="thread-dock">
            {!atBottom && (
              <button
                className="jump-latest"
                onClick={() => {
                  const element = scroll.current;
                  if (!element) return;
                  const reduced = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
                  jumping.current = !reduced;
                  pinned.current = true;
                  setAtBottom(true);
                  element.focus({ preventScroll: true });
                  element.scrollTo({
                    top: element.scrollHeight,
                    behavior: reduced ? 'instant' : 'smooth',
                  });
                }}
              >
                <ArrowDown size={14} />
                {t('回到最新', 'Jump to latest')}
              </button>
            )}
            {local ? (
              <Composer inputRef={textarea} />
            ) : (
              <div className="readonly-footer">
                <LockKeyhole size={15} />
                <div>
                  <strong>{t('渠道会话只读', 'Read-only channel conversation')}</strong>
                  <span>
                    {t(
                      '发送消息、审批和任务控制请在原渠道进行。',
                      'Send messages, approve actions and control tasks in the original channel.',
                    )}
                  </span>
                </div>
              </div>
            )}
          </div>
        </>
      )}
    </div>
  );
}
