import { useState } from 'react';
import { Cable, Settings2, Wifi, WifiOff } from 'lucide-react';
import { useApp } from '../context.ts';
import { Modal, PlatformIcon } from '../components.tsx';
import { PageFrame } from '../layout.tsx';
import type { Connection } from '../schema.ts';

export function ConnectionsPage() {
  const { state, dispatch, t, selectConversation } = useApp();
  const [editing, setEditing] = useState<Connection | null>(null);
  const [account, setAccount] = useState('');
  const [simulateFailure, setSimulateFailure] = useState(false);
  const labels = {
    connected: t('已连接', 'Connected'),
    connecting: t('连接中', 'Connecting'),
    disconnected: t('未连接', 'Disconnected'),
    error: t('连接异常', 'Connection error'),
  };

  return (
    <PageFrame
      title={t('连接', 'Connections')}
      detail={t(
        '管理消息渠道连接。各渠道的会话与权限独立。',
        'Manage channel connections. Conversations and permissions remain separate.',
      )}
    >
      <div className="connection-list">
        {state.connections.map((connection) => {
          const conversations = state.conversations.filter((c) => c.connectionId === connection.id);
          return (
            <article className="connection-card" key={connection.id}>
              <PlatformIcon platform={connection.platform} size={18} />
              <div className="connection-main">
                <h2>
                  <button
                    disabled={!conversations.length}
                    title={t('查看会话', 'View conversations')}
                    onClick={() => selectConversation(conversations[0].id)}
                  >
                    {connection.name}
                  </button>
                </h2>
                <p>
                  {connection.account}
                  <span>
                    {t(
                      `${conversations.length} 段独立会话`,
                      `${conversations.length} ${conversations.length === 1 ? 'conversation' : 'conversations'}`,
                    )}
                  </span>
                </p>
              </div>
              <span className={`connection-status ${connection.status}`}>
                <span />
                {labels[connection.status]}
              </span>
              <div className="connection-actions">
                <button
                  className="button secondary small"
                  onClick={() => {
                    setEditing(connection);
                    setAccount(connection.account);
                    setSimulateFailure(connection.simulateFailure);
                  }}
                >
                  <Settings2 size={13} />
                  {t('配置', 'Configure')}
                </button>
                {connection.status === 'connected' || connection.status === 'connecting' ? (
                  <button
                    className="button ghost small"
                    onClick={() => dispatch({ type: 'disconnect', connectionId: connection.id })}
                  >
                    <WifiOff size={13} />
                    {t('断开', 'Disconnect')}
                  </button>
                ) : (
                  <button
                    className="button primary small"
                    onClick={() => dispatch({ type: 'connect', connectionId: connection.id })}
                  >
                    <Wifi size={13} />
                    {t('连接', 'Connect')}
                  </button>
                )}
              </div>
            </article>
          );
        })}
      </div>
      <div className="page-callout">
        <Cable size={17} />
        <div>
          <h3>{t('共享工作区，会话隔离', 'Shared workspace, isolated conversations')}</h3>
          <p>
            {t(
              '共享工作区和公共记忆，让不同入口的助手有相同的基础。个人偏好和聊天上下文各自保存。应用关闭后，连接和执行一起停止。',
              'Your workspace and shared memory provide common ground. Personal preferences and conversation context stay separate. Closing the app stops connections and execution.',
            )}
          </p>
        </div>
      </div>
      {editing && (
        <Modal
          title={t('配置演示连接', 'Configure demo connection')}
          close={() => setEditing(null)}
        >
          <div className="modal-body">
            <div className="connection-modal-brand">
              <PlatformIcon platform={editing.platform} size={18} />
              <span>
                <strong>{editing.name}</strong>
                <small>{t('演示连接', 'Demo connection')}</small>
              </span>
            </div>
            <label className="form-field">
              {t('示例账号名称', 'Sample account name')}
              <input value={account} onChange={(e) => setAccount(e.target.value)} />
            </label>
            <label className="form-field">
              {t('演示连接结果', 'Demo connection result')}
              <select
                value={simulateFailure ? 'error' : 'success'}
                onChange={(e) => setSimulateFailure(e.target.value === 'error')}
              >
                <option value="success">{t('连接成功', 'Successful connection')}</option>
                <option value="error">{t('连接异常', 'Connection error')}</option>
              </select>
            </label>
            <p className="field-help">
              {t(
                '这里只保存演示配置，不会连接真实账号，也不需要密钥。',
                'This saves a demo configuration. No real account is connected and no key is needed.',
              )}
            </p>
            <div className="modal-actions">
              <button className="button secondary" onClick={() => setEditing(null)}>
                {t('取消', 'Cancel')}
              </button>
              <button
                className="button primary"
                disabled={!account.trim()}
                onClick={() => {
                  dispatch({
                    type: 'connect',
                    connectionId: editing.id,
                    account: account.trim(),
                    simulateFailure,
                  });
                  setEditing(null);
                }}
              >
                {t('保存并模拟连接', 'Save and simulate connection')}
              </button>
            </div>
          </div>
        </Modal>
      )}
    </PageFrame>
  );
}
