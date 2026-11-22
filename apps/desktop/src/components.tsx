import { lazy, Suspense, useEffect, useRef, type ReactNode } from 'react';
import {
  ArrowUpRight,
  Check,
  CircleAlert,
  CircleCheck,
  CircleDashed,
  CirclePause,
  CircleSlash,
  Clock3,
  FileText,
  Inbox,
  LoaderCircle,
  LockKeyhole,
  Send,
  X,
  type LucideIcon,
} from 'lucide-react';
import { useApp } from './context.ts';
import type { Connection, Status, Task } from './schema.ts';
import { canControl } from './model.ts';

export function Logo() {
  return (
    <span className="logo" aria-hidden="true">
      o<span>.</span>
    </span>
  );
}

export function PlatformIcon({
  platform,
  size = 16,
}: {
  platform: Connection['platform'];
  size?: number;
}) {
  if (platform === 'telegram')
    return (
      <span className="platform telegram">
        <Send size={size} />
      </span>
    );
  if (platform === 'qq')
    return (
      <span className="platform qq">
        <svg width={size} height={size} viewBox="0 0 24 24" aria-hidden="true">
          <path
            d="M7 10C5 3 19 3 17 10c3 3 4 9 2 8l-2-1c0 4-10 4-10 0l-2 1c-2 1-1-5 2-8Z"
            fill="currentColor"
          />
          <path
            d="M8 13h8m-7 6-2 2m8-2 2 2"
            stroke="#edb57d"
            strokeWidth="2"
            strokeLinecap="round"
          />
          <circle cx="10" cy="9" r="1" fill="white" />
          <circle cx="14" cy="9" r="1" fill="white" />
        </svg>
      </span>
    );
  return (
    <span className="platform feishu">
      <svg width={size} height={size} viewBox="0 0 24 24" fill="none" aria-hidden="true">
        <path d="m3 5 10 7-5 5-5-2V5Z" fill="#4285db" />
        <path d="m7 19 8-8 6-3v8l-8 5-6-2Z" fill="#4eb2bd" />
      </svg>
    </span>
  );
}

const statuses: Record<Status, [LucideIcon, string, string]> = {
  queued: [CircleDashed, '准备中', 'Queued'],
  running: [LoaderCircle, '进行中', 'Working'],
  awaiting_approval: [Clock3, '待确认', 'Needs approval'],
  cancelling: [LoaderCircle, '正在停止', 'Stopping'],
  completed: [CircleCheck, '已完成', 'Completed'],
  failed: [CircleAlert, '遇到问题', 'Needs attention'],
  cancelled: [CircleSlash, '已停止', 'Stopped'],
  interrupted: [CirclePause, '已中断', 'Interrupted'],
};
export function StatusBadge({ status }: { status: Status }) {
  const { t } = useApp();
  const [Icon, zh, en] = statuses[status];
  return (
    <span className={`status ${status}`}>
      <Icon size={13} className={status === 'running' ? 'spin' : undefined} />
      {t(zh, en)}
    </span>
  );
}

type MemberState = Task['members'][number]['state'];
const memberStates: Record<MemberState, [LucideIcon, string, string]> = {
  waiting: [CircleDashed, '等待中', 'Waiting'],
  working: [LoaderCircle, '工作中', 'Working'],
  done: [CircleCheck, '已完成', 'Done'],
  failed: [CircleAlert, '遇到问题', 'Needs attention'],
  stopped: [CircleSlash, '已停止', 'Stopped'],
};
export function useMemberState() {
  const { t } = useApp();
  return (state: MemberState) => {
    const [Icon, zh, en] = memberStates[state];
    return {
      label: t(zh, en),
      icon: <Icon size={13} className={state === 'working' ? 'spin' : undefined} />,
    };
  };
}

export function Time({
  value,
  format,
}: {
  value: number;
  format: 'time' | 'date' | 'datetime' | 'day';
}) {
  const { state, t } = useApp();
  const locale = state.settings.language === 'zh' ? 'zh-CN' : 'en';
  const date = new Date(value);
  const clock = { hour: '2-digit', minute: '2-digit', hour12: false } as const;
  let text: string;
  if (format === 'day') {
    const today = new Date();
    const days = Math.round(
      (new Date(today).setHours(0, 0, 0, 0) - new Date(value).setHours(0, 0, 0, 0)) / 86_400_000,
    );
    text =
      days === 0
        ? t('今天', 'Today')
        : days === 1
          ? t('昨天', 'Yesterday')
          : new Intl.DateTimeFormat(locale, {
              year: date.getFullYear() === today.getFullYear() ? undefined : 'numeric',
              month: 'long',
              day: 'numeric',
              weekday: 'short',
            }).format(date);
  } else {
    text = new Intl.DateTimeFormat(
      locale,
      format === 'time'
        ? clock
        : format === 'date'
          ? { month: 'short', day: 'numeric' }
          : { month: 'short', day: 'numeric', ...clock },
    ).format(date);
  }
  return <time dateTime={date.toISOString()}>{text}</time>;
}

const RichText = lazy(() => import('./RichText.tsx'));
export function Markdown({ children }: { children: string }) {
  return (
    <div className="markdown">
      <Suspense
        fallback={
          <div className="rich-text-loading" aria-busy="true">
            <i />
            <i />
            <i />
          </div>
        }
      >
        <RichText>{children}</RichText>
      </Suspense>
    </div>
  );
}

export function Modal({
  title,
  children,
  close,
  variant,
}: {
  title: string;
  children: ReactNode;
  close: () => void;
  variant?: 'palette';
}) {
  const dialog = useRef<HTMLDialogElement>(null);
  const previousFocus = useRef(document.activeElement as HTMLElement | null);
  useEffect(() => {
    const element = dialog.current;
    element?.showModal();
    element?.querySelector<HTMLElement>('input, textarea, [data-autofocus]')?.focus();
    return () => {
      element?.close();
      if (previousFocus.current?.isConnected) previousFocus.current.focus();
    };
  }, []);
  return (
    <dialog
      ref={dialog}
      className={`modal ${variant ?? ''}`}
      aria-label={title}
      onCancel={(e) => {
        e.preventDefault();
        close();
      }}
      onClick={(e) => {
        if (e.target === e.currentTarget) close();
      }}
    >
      {variant !== 'palette' && (
        <div className="modal-header">
          <h2>{title}</h2>
          <CloseButton onClick={close} />
        </div>
      )}
      {children}
    </dialog>
  );
}

export function CloseButton({ onClick }: { onClick: () => void }) {
  const { t } = useApp();
  return (
    <button className="icon-button" aria-label={t('关闭', 'Close')} onClick={onClick}>
      <X size={17} />
    </button>
  );
}

export function Switch({
  checked,
  label,
  onChange,
  showLabel = false,
}: {
  checked: boolean;
  label: string;
  onChange: (checked: boolean) => void;
  showLabel?: boolean;
}) {
  return (
    <button
      className="switch"
      role="switch"
      aria-checked={checked}
      aria-label={showLabel ? undefined : label}
      onClick={() => onChange(!checked)}
    >
      <span className="switch-track">
        <i />
      </span>
      {showLabel && label}
    </button>
  );
}

export function Empty({
  title,
  detail,
  icon,
}: {
  title: string;
  detail: string;
  icon?: ReactNode;
}) {
  return (
    <div className="empty-state">
      {icon ?? <Inbox size={24} />}
      <h3>{title}</h3>
      <p>{detail}</p>
    </div>
  );
}

export function Source({ conversationId }: { conversationId: string }) {
  const { state, t } = useApp();
  const c = state.conversations.find((c) => c.id === conversationId);
  const connection = state.connections.find((v) => v.id === c?.connectionId);
  return (
    <span className="source">
      <span className={`source-dot ${connection?.platform ?? 'desktop'}`} />
      {c?.origin === 'desktop' ? t('桌面', 'Desktop') : `${connection?.name} · ${c?.name}`}
    </span>
  );
}

export function TaskCard({ task }: { task: Task }) {
  const { state, t, openTask } = useApp();
  return (
    <button
      className="task-card"
      onClick={() => openTask(task.id)}
      data-testid={`task-card-${task.id}`}
    >
      <span className="task-card-title">{task.title}</span>
      {task.artifact && (
        <span className="task-card-artifact" title={t('成果已就绪', 'Artifact ready')}>
          <FileText size={13} aria-hidden="true" />
          <span className="visually-hidden">{t('成果已就绪', 'Artifact ready')}</span>
        </span>
      )}
      <StatusBadge status={task.status} />
      {canControl(state, task.conversationId) ? (
        <ArrowUpRight size={14} />
      ) : (
        <LockKeyhole size={13} />
      )}
    </button>
  );
}

export function Approval({ task }: { task: Task }) {
  const { state, dispatch, t } = useApp();
  if (!task.approval) return null;
  const local = canControl(state, task.conversationId);
  const pending = task.approval.status === 'pending';
  const approvalLabels = {
    pending: t('需要你的确认', 'Needs your approval'),
    approved: t('已允许此操作', 'Action approved'),
    rejected: t('已拒绝此操作', 'Action declined'),
    expired: t('此请求已失效', 'Request expired'),
    cancelled: t('此请求已取消', 'Request cancelled'),
  };
  return (
    <div className={`approval-card ${pending ? '' : 'settled'}`} data-testid="approval-card">
      <div className="approval-heading">
        {pending ? (
          <LockKeyhole size={15} />
        ) : task.approval.status === 'approved' ? (
          <Check size={15} />
        ) : (
          <CircleSlash size={15} />
        )}
        <strong>{approvalLabels[task.approval.status]}</strong>
      </div>
      <p className="approval-detail">
        {t('将整理后的笔记写入 ', 'Write the organized note to ')}
        <code>{task.approval.target}</code>
        {t('，原始文件保持不变。', ' and keep the originals.')}
      </p>
      {pending && local && (
        <div className="approval-actions">
          <button
            className="button secondary"
            onClick={() =>
              dispatch({ type: 'reject', taskId: task.id, approvalId: task.approval!.id })
            }
          >
            {t('拒绝', 'Decline')}
          </button>
          <button
            className="button primary"
            disabled={state.settings.permission === 'deny'}
            onClick={() =>
              dispatch({ type: 'approve', taskId: task.id, approvalId: task.approval!.id })
            }
          >
            {t('允许这次操作', 'Allow this action')}
          </button>
        </div>
      )}
      {!local && (
        <div className="read-only-note">
          <LockKeyhole size={12} />
          {t('仅供预览 · 请在原渠道处理', 'Preview only · Respond in the original channel')}
        </div>
      )}
    </div>
  );
}
