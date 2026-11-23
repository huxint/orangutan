import { useState } from 'react';
import { BellRing, Check, Clock3, Plus, Trash2 } from 'lucide-react';
import { useApp } from '../context.ts';
import { Empty, Time } from '../components.tsx';
import { PageFrame } from '../layout.tsx';
import { PetAvatar } from '../pets.tsx';

export function RemindersPage() {
  const { state, dispatch, t, toast } = useApp();
  const [title, setTitle] = useState('');
  const [offsetMinutes, setOffsetMinutes] = useState(30);
  const companions = state.settings.companions !== false;
  const reminders = state.reminders ?? [];
  const pending = reminders.filter((r) => !r.completed);
  const done = reminders.filter((r) => r.completed);

  const add = () => {
    const clean = title.trim();
    if (!clean) return;
    const now = Date.now();
    dispatch({
      type: 'reminder-add',
      id: crypto.randomUUID(),
      title: clean,
      dueAt: now + offsetMinutes * 60_000,
      now,
    });
    setTitle('');
    toast(t('已添加到个人提醒', 'Reminder scheduled'));
  };

  return (
    <PageFrame
      title={t('提醒', 'Reminders')}
      detail={t(
        '记录待办与时间节点；连接本地运行时后，到点由 Pip 准时提醒。',
        'Keep track of upcoming personal reminders. When connected to the local runtime, Pip notifies you on time.',
      )}
    >
      <form
        className="reminder-composer"
        onSubmit={(e) => {
          e.preventDefault();
          add();
        }}
      >
        <input
          aria-label={t('新提醒内容', 'New reminder')}
          placeholder={t(
            '例如：30 分钟后复盘周报、今晚读两章书…',
            'e.g. Review weekly notes in 30 min…',
          )}
          value={title}
          maxLength={240}
          onChange={(e) => setTitle(e.target.value)}
        />
        <select
          aria-label={t('提醒时间', 'Reminder delay')}
          value={offsetMinutes}
          onChange={(e) => setOffsetMinutes(Number(e.target.value))}
        >
          <option value={15}>{t('15 分钟后', 'In 15 min')}</option>
          <option value={30}>{t('30 分钟后', 'In 30 min')}</option>
          <option value={60}>{t('1 小时后', 'In 1 hour')}</option>
          <option value={240}>{t('4 小时后', 'In 4 hours')}</option>
          <option value={1440}>{t('明天此时', 'Tomorrow')}</option>
        </select>
        <button className="button primary" disabled={!title.trim()}>
          <Plus size={14} />
          {t('添加提醒', 'Add')}
        </button>
      </form>

      {pending.length > 0 && (
        <div className="reminder-list">
          {pending.map((item) => (
            <div className="reminder-row" key={item.id}>
              <button
                className="reminder-check"
                aria-label={t('标记完成', 'Mark completed')}
                onClick={() => dispatch({ type: 'reminder-toggle', id: item.id })}
              />
              <div className="reminder-main">
                <strong>{item.title}</strong>
                <small>
                  <Clock3 size={12} />
                  <Time value={item.dueAt} format="datetime" />
                </small>
              </div>
              <button
                className="icon-button small"
                aria-label={t('删除提醒', 'Delete reminder')}
                onClick={() => dispatch({ type: 'reminder-delete', id: item.id })}
              >
                <Trash2 size={14} />
              </button>
            </div>
          ))}
        </div>
      )}

      {done.length > 0 && (
        <div className="reminder-list is-done">
          {done.map((item) => (
            <div className="reminder-row done" key={item.id}>
              <button
                className="reminder-check checked"
                aria-label={t('取消完成', 'Mark incomplete')}
                onClick={() => dispatch({ type: 'reminder-toggle', id: item.id })}
              >
                <Check size={12} />
              </button>
              <div className="reminder-main">
                <strong>{item.title}</strong>
                <small>
                  <Time value={item.dueAt} format="datetime" />
                </small>
              </div>
              <button
                className="icon-button small"
                aria-label={t('删除提醒', 'Delete reminder')}
                onClick={() => dispatch({ type: 'reminder-delete', id: item.id })}
              >
                <Trash2 size={14} />
              </button>
            </div>
          ))}
        </div>
      )}

      {!reminders.length && (
        <Empty
          icon={companions ? <PetAvatar pet="pip" size={44} /> : <BellRing size={24} />}
          title={t('还没有提醒事项', 'No reminders yet')}
          detail={t(
            '在上方记下一件小事，啾啾会在到点时提醒你。',
            'Add a reminder above and Pip will bring it up right on time.',
          )}
        />
      )}
    </PageFrame>
  );
}
