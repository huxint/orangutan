import { useState } from 'react';
import { ChevronRight, FileText, LockKeyhole, Users } from 'lucide-react';
import { useApp } from '../context.ts';
import { active } from '../model.ts';
import { Empty, Source, StatusBadge, Time } from '../components.tsx';
import { SearchField, Segmented } from '../controls.tsx';
import { PageFrame, Toolbar } from '../layout.tsx';

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
