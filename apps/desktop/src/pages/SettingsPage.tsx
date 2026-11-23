import { useState } from 'react';
import {
  ArrowDownToLine,
  FolderOpen,
  Monitor,
  Moon,
  Settings2,
  ShieldCheck,
  SlidersHorizontal,
  Sparkles,
  Sun,
} from 'lucide-react';
import { useApp } from '../context.ts';
import { Logo, Modal, Switch } from '../components.tsx';
import { SettingRow } from '../controls.tsx';
import { PageFrame } from '../layout.tsx';
import { PetAvatar } from '../pets.tsx';

export function SettingsPage({
  reset,
  exportSnapshot,
}: {
  reset: () => void;
  exportSnapshot: () => void;
}) {
  const { state, runtime, dispatch, t, toast } = useApp();
  const [confirmReset, setConfirmReset] = useState(false);
  const companions = state.settings.companions !== false;
  const update = (value: Partial<typeof state.settings>) => dispatch({ type: 'settings', value });

  return (
    <PageFrame
      title={t('设置', 'Settings')}
      detail={t(
        '配置外观、森林伙伴陪伴、模型与桌面对话的操作权限。',
        'Configure appearance, Forest Companions, model and local conversation permissions.',
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
        <SettingRow
          title={t('森林伙伴陪伴', 'Forest Companions')}
          detail={t(
            '让 Oran、Scout、Moss、Fern、Pebble 与 Pip 随运行状态温柔陪伴；关闭后回到极简文字。',
            'Show Oran, Scout, Moss, Fern, Pebble and Pip reacting quietly to live work. Turn off for minimal text.',
          )}
        >
          <Switch
            checked={companions}
            label={t('森林伙伴陪伴', 'Forest Companions')}
            onChange={(value) => update({ companions: value })}
          />
        </SettingRow>
      </section>

      {companions && (
        <section className="settings-section">
          <h2>
            <Sparkles size={15} />
            {t('你的森林伙伴', 'Your Forest Companions')}
          </h2>
          <div className="companion-roster">
            {(
              [
                ['oran', t('奥兰 · 主助手', 'Oran · Lead')],
                ['scout', t('寻叶 · 检索', 'Scout · Explore')],
                ['moss', t('苔苔 · 记忆', 'Moss · Memory')],
                ['fern', t('蕨蕨 · 工匠', 'Fern · Builder')],
                ['pebble', t('石子 · 守护', 'Pebble · Guard')],
                ['pip', t('啾啾 · 信使', 'Pip · Messenger')],
              ] as const
            ).map(([pet, label]) => (
              <div className="companion-chip" key={pet}>
                <PetAvatar pet={pet} size={34} />
                <span>{label}</span>
              </div>
            ))}
          </div>
        </section>
      )}

      <section className="settings-section">
        <h2>
          <Settings2 size={15} />
          {t('助手偏好', 'Assistant preferences')}
        </h2>
        <SettingRow
          title={t('运行时连接', 'Runtime connection')}
          detail={
            runtime.mode === 'offline'
              ? t(
                  '未检测到本地 C++26 宿主，当前运行在离线演示模式。启动 `orangutan serve` 即可自动接入真实运行时。',
                  'No local C++26 host detected; running in offline demo mode. Start `orangutan serve` to connect live.',
                )
              : t(
                  `已连接本地 C++26 宿主（模型：${runtime.model ?? 'local'}）。`,
                  `Connected to local C++26 host (model: ${runtime.model ?? 'local'}).`,
                )
          }
        >
          <code className="path-chip">
            {runtime.mode === 'offline'
              ? t('离线演示', 'Offline demo')
              : runtime.mode === 'live-demo'
                ? t('本地宿主 · 离线模型', 'Local host · Demo model')
                : t('本地宿主 · 实机模型', 'Local host · Live')}
          </code>
        </SettingRow>
        <SettingRow
          title={t('示例模型', 'Sample model')}
          detail={t(
            '离线演示时展示模型选择；连接本地宿主时以配置文件为准。',
            'Demo model picker when offline; live host uses your config file.',
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
          title={t('文件与命令操作', 'File & command actions')}
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
            <option value="allow">{t('允许演示操作', 'Allow actions')}</option>
            <option value="deny">{t('拒绝文件操作', 'Deny file actions')}</option>
          </select>
        </SettingRow>
      </section>

      <section className="settings-section">
        <h2>
          <FolderOpen size={15} />
          {t('工作区与本地数据', 'Workspace & local data')}
        </h2>
        <SettingRow
          title={t('工作区路径', 'Workspace')}
          detail={
            runtime.workspace
              ? t('已挂载至本地 C++26 宿主工作区。', 'Mounted by the local C++26 host.')
              : t(
                  '离线演示空间，不读取本机文件。',
                  'Offline demo space. Local files are not accessed.',
                )
          }
        >
          <code className="path-chip">{runtime.workspace ?? 'workspace/'}</code>
        </SettingRow>
        <SettingRow
          title={t('本地数据', 'Local data')}
          detail={t(
            '离线演示保存在浏览器中，连接本地宿主时写入 SQLite。',
            'Offline demo saves in this browser; live host persists to SQLite.',
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
          Orangutan <span aria-hidden="true">·</span>{' '}
          {runtime.mode === 'offline'
            ? t('桌面原型', 'Desktop prototype')
            : t('Personal Agent 本地工作台', 'Personal Agent Workbench')}
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
