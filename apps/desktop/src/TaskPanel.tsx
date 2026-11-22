import { useEffect, useRef, useState } from 'react';
import {
  Activity,
  ChevronRight,
  CircleAlert,
  CircleCheck,
  FileText,
  LockKeyhole,
  MessageCircle,
  RotateCcw,
  Square,
  Users,
} from 'lucide-react';
import { useApp } from './context.ts';
import { active, canControl } from './model.ts';
import {
  Approval,
  CloseButton,
  Markdown,
  Source,
  StatusBadge,
  Time,
  useMemberState,
} from './components.tsx';
import { Inspector } from './layout.tsx';

export function TaskPanel({ id, close }: { id: string; close: () => void }) {
  const { state, dispatch, selectConversation, t } = useApp();
  const memberState = useMemberState();
  const task = state.tasks.find((task) => task.id === id);
  const [tab, setTab] = useState<'result' | 'progress' | 'team'>(
    task?.artifact ? 'result' : 'progress',
  );
  const [raw, setRaw] = useState(false);
  const panel = useRef<HTMLElement>(null);
  const previousFocus = useRef(document.activeElement as HTMLElement | null);
  useEffect(() => {
    panel.current?.querySelector<HTMLButtonElement>('button')?.focus();
    return () => {
      const previous = previousFocus.current;
      if (previous?.isConnected && previous.getClientRects().length) previous.focus();
      else document.getElementById('main-content')?.focus();
    };
  }, []);
  if (!task) return null;
  const local = canControl(state, task.conversationId);
  const executionSummary = {
    queued: t('任务已进入队列，尚未开始执行。', 'This task is queued and has not started.'),
    running: t(
      '任务正在执行。完成后，回复与成果会保留在所属会话。',
      'Work is in progress. The response and any artifact will remain in this conversation.',
    ),
    awaiting_approval: t(
      '执行已暂停，等待确认下方的文件操作。',
      'Execution is paused for the file action below.',
    ),
    cancelling: t(
      '已请求停止，正在等待本次执行结束。',
      'Stop requested. Waiting for this run to finish.',
    ),
    completed: task.artifact
      ? t(
          '执行已结束，成果可在「成果」中查看。',
          'The run has finished. Open Result to review the artifact.',
        )
      : t(
          '执行已结束，回复已保留在会话中。',
          'The run has finished. The response is in the conversation.',
        ),
    failed: t(
      '本次执行未完成，请查看下方说明。',
      'This run did not finish. See the details below.',
    ),
    cancelled: t(
      '本次执行已停止，已有会话记录仍然保留。',
      'This run has stopped. Existing conversation history is preserved.',
    ),
    interrupted: t(
      '本次执行已中断，尚未重新开始。',
      'This run was interrupted and has not restarted.',
    ),
  }[task.status];
  const tabs = [
    { id: 'result' as const, icon: FileText, label: t('成果', 'Result') },
    { id: 'progress' as const, icon: Activity, label: t('进度', 'Progress') },
    ...(task.members.length
      ? [{ id: 'team' as const, icon: Users, label: t('小队', 'Team') }]
      : []),
  ];
  const footer = !local ? (
    <span>
      <LockKeyhole size={13} />
      {t('渠道任务仅供预览', 'Channel task · Read only')}
    </span>
  ) : (
    <>
      <span>{t('本地工作区', 'Local workspace')}</span>
      {active(task.status) ? (
        <button
          className="button secondary small"
          disabled={task.status === 'cancelling'}
          onClick={() => dispatch({ type: 'stop', taskId: task.id, now: Date.now() })}
        >
          <Square size={11} />
          {t('停止任务', 'Stop task')}
        </button>
      ) : ['failed', 'cancelled', 'interrupted'].includes(task.status) ? (
        <button
          className="button secondary small"
          onClick={() => dispatch({ type: 'retry', taskId: task.id, now: Date.now() })}
        >
          <RotateCcw size={12} />
          {t('重新尝试', 'Try again')}
        </button>
      ) : (
        <span>
          <CircleCheck size={13} />
          {t('已完成', 'Completed')}
        </span>
      )}
    </>
  );
  return (
    <Inspector
      label={t('任务详情', 'Task details')}
      testId="task-panel"
      innerRef={panel}
      header={
        <>
          <span>{t('任务', 'Task')}</span>
          <CloseButton onClick={close} />
        </>
      }
      footer={footer}
    >
      <div className="panel-title">
        <Source conversationId={task.conversationId} />
        <h2>{task.title}</h2>
        <div className="panel-meta">
          <StatusBadge status={task.status} />
          <button className="text-button" onClick={() => selectConversation(task.conversationId)}>
            <MessageCircle size={14} />
            {t('查看所属对话', 'Open conversation')}
          </button>
        </div>
      </div>
      <div className="segmented panel-tabs" role="group" aria-label={t('详情视图', 'Detail view')}>
        {tabs.map(({ id, icon: Icon, label }) => (
          <button key={id} aria-pressed={tab === id} onClick={() => setTab(id)}>
            <Icon size={14} />
            {label}
          </button>
        ))}
      </div>
      <div className="panel-content" key={tab}>
        {tab === 'result' &&
          (task.artifact ? (
            <>
              <div className="artifact-toolbar">
                <span className="artifact-name">
                  <FileText size={14} />
                  <span>{task.artifact.name}</span>
                </span>
                <div
                  className="segmented"
                  role="group"
                  aria-label={t('成果显示方式', 'Artifact view')}
                >
                  <button aria-pressed={!raw} onClick={() => setRaw(false)}>
                    {t('预览', 'Preview')}
                  </button>
                  <button aria-pressed={raw} onClick={() => setRaw(true)}>
                    {t('原文', 'Source')}
                  </button>
                </div>
              </div>
              <article className="artifact-paper" key={raw ? 'source' : 'preview'}>
                {raw ? (
                  <pre>
                    <code>{task.artifact.body}</code>
                  </pre>
                ) : (
                  <Markdown>{task.artifact.body}</Markdown>
                )}
              </article>
              <div className="artifact-footnote">
                <span className="source-dot desktop" />
                {t('演示工作区 · 仅供预览', 'Demo workspace · Preview only')}
              </div>
            </>
          ) : (
            <div className="panel-empty">
              <FileText size={22} />
              <h3>{t('暂无成果', 'No artifact yet')}</h3>
              <p>
                {t(
                  '任务完成后，可以在这里查看文档、文件和整理好的方案。',
                  'Documents, files and plans will appear here when the task is finished.',
                )}
              </p>
            </div>
          ))}
        {tab === 'progress' && (
          <>
            <section className="execution-summary" aria-label={t('执行状态', 'Execution status')}>
              <p>{executionSummary}</p>
              <dl className="facts">
                <div>
                  <dt>{t('当前尝试', 'Current attempt')}</dt>
                  <dd>#{task.attempt}</dd>
                </div>
                <div>
                  <dt>{t('发起时间', 'Requested')}</dt>
                  <dd>
                    <Time value={task.createdAt} format="datetime" />
                  </dd>
                </div>
              </dl>
            </section>
            <Approval task={task} />
            {task.status === 'failed' && (
              <div className="error-note">
                <CircleAlert size={15} />
                {task.approval?.status === 'rejected'
                  ? t(
                      '操作已被拒绝，未产生文件变更。',
                      'The action was declined. No file change was made.',
                    )
                  : t(
                      '示例执行未完成。可以查看小队记录，或在所属入口重新发起。',
                      'The demo run did not complete. Review the team notes or retry from its own channel.',
                    )}
              </div>
            )}
            {task.status === 'interrupted' && (
              <div className="error-note">
                <CircleAlert size={15} />
                {t(
                  '上次页面关闭时，这项工作还没有完成。历史已保留，未自动重新执行。',
                  'This work was unfinished when the page closed. History is kept; it has not been restarted.',
                )}
              </div>
            )}
            {task.history.length > 0 && (
              <div className="attempt-history">
                <h4>{t('之前的执行', 'Previous attempts')}</h4>
                {task.history.map((h) => (
                  <div key={h.attempt}>
                    <span>#{h.attempt}</span>
                    <StatusBadge status={h.status} />
                  </div>
                ))}
              </div>
            )}
          </>
        )}
        {tab === 'team' && (
          <>
            <p className="panel-intro">
              {t(
                '查看成员的分工、当前状态与已有结果。',
                'Review each member’s assignment, current state and recorded result.',
              )}
            </p>
            <div className="team-members">
              {task.members.map((member) => {
                const { label, icon } = memberState(member.state);
                return (
                  <details className="team-member" key={member.id} open={member.state === 'failed'}>
                    <summary>
                      <span className="team-member-name">
                        <strong>{member.role}</strong>
                        <span>{member.name}</span>
                      </span>
                      <span className={`member-state ${member.state}`}>
                        {icon}
                        {label}
                      </span>
                      <ChevronRight size={14} />
                    </summary>
                    <p>{member.result || t('暂无结果记录。', 'No result recorded yet.')}</p>
                  </details>
                );
              })}
            </div>
          </>
        )}
      </div>
    </Inspector>
  );
}
