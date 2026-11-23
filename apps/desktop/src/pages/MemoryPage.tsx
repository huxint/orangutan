import { useState } from 'react';
import { Globe2, Leaf, LockKeyhole, Pencil, Plus, ShieldCheck, Trash2 } from 'lucide-react';
import { useApp } from '../context.ts';
import { Empty, Modal, Switch, Time } from '../components.tsx';
import { SearchField, Segmented } from '../controls.tsx';
import { PageFrame, Toolbar } from '../layout.tsx';
import { PetAvatar } from '../pets.tsx';
import { requesterKey, type Note } from '../schema.ts';

export function MemoryPage() {
  const { state, dispatch, t, toast } = useApp();
  const [query, setQuery] = useState('');
  const [scope, setScope] = useState('all');
  const [source, setSource] = useState('all');
  const [editing, setEditing] = useState<Note | null>(null);
  const [creating, setCreating] = useState<{
    scope: 'personal' | 'shared';
    title: string;
    body: string;
  } | null>(null);
  const [deleting, setDeleting] = useState(false);
  const companions = state.settings.companions !== false;
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
        <div className="setting-actions">
          <button
            className="button secondary small"
            onClick={() => setCreating({ scope: 'personal', title: '', body: '' })}
          >
            <Plus size={14} />
            {t('新增记忆', 'New note')}
          </button>
          <Switch
            checked={state.settings.memory}
            label={t('自动记忆', 'Auto memory')}
            showLabel
            onChange={(memory) => dispatch({ type: 'settings', value: { memory } })}
          />
        </div>
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
          icon={companions ? <PetAvatar pet="moss" size={44} /> : <Leaf size={24} />}
          title={t('还没有这样的记忆', 'Nothing here just yet')}
          detail={t(
            '换一个搜索词，或在对话里告诉苔苔值得记住的事。',
            'Try another search, or tell Moss something worth remembering.',
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
      {creating && (
        <Modal title={t('新增长期记忆', 'Add long-term note')} close={() => setCreating(null)}>
          <div className="modal-body">
            <label className="form-field">
              {t('作用域', 'Scope')}
              <select
                value={creating.scope}
                onChange={(e) =>
                  setCreating({ ...creating, scope: e.target.value as 'personal' | 'shared' })
                }
              >
                <option value="personal">{t('个人记忆', 'Personal')}</option>
                <option value="shared">{t('共享基础', 'Shared workspace')}</option>
              </select>
            </label>
            <label className="form-field">
              {t('标题', 'Title')}
              <input
                value={creating.title}
                placeholder={t('例如：回复风格偏好', 'e.g. Response style preference')}
                onChange={(e) => setCreating({ ...creating, title: e.target.value })}
              />
            </label>
            <label className="form-field">
              {t('内容', 'Content')}
              <textarea
                rows={4}
                value={creating.body}
                placeholder={t(
                  '写下希望助手长期记住的事实或偏好…',
                  'Write a durable preference or fact…',
                )}
                onChange={(e) => setCreating({ ...creating, body: e.target.value })}
              />
            </label>
            <div className="modal-actions">
              <button className="button secondary" onClick={() => setCreating(null)}>
                {t('取消', 'Cancel')}
              </button>
              <button
                className="button primary"
                disabled={!creating.title.trim() || !creating.body.trim()}
                onClick={() => {
                  dispatch({
                    type: 'note-add',
                    note: {
                      id: crypto.randomUUID(),
                      scope: creating.scope,
                      ownerId: creating.scope === 'shared' ? null : requesterKey('desktop', 'you'),
                      title: creating.title.trim(),
                      body: creating.body.trim(),
                      source:
                        creating.scope === 'shared'
                          ? t('共享工作区', 'Shared workspace')
                          : t('桌面 · 手动记录', 'Desktop · Manual'),
                      updatedAt: Date.now(),
                    },
                  });
                  setCreating(null);
                  toast(t('已保存新记忆', 'Note saved'));
                }}
              >
                {t('保存', 'Save')}
              </button>
            </div>
          </div>
        </Modal>
      )}
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
