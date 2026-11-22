import type { ReactNode, Ref } from 'react';

export function AppShell({
  sidebarOpen,
  hasPanel = false,
  sidebar,
  onCloseSidebar,
  closeLabel,
  children,
}: {
  sidebarOpen: boolean;
  hasPanel?: boolean;
  sidebar: ReactNode;
  onCloseSidebar: () => void;
  closeLabel: string;
  children: ReactNode;
}) {
  return (
    <div
      className={`app-shell${hasPanel ? ' has-panel' : ''}${sidebarOpen ? ' sidebar-open' : ''}`}
    >
      {sidebarOpen && (
        <button className="sidebar-scrim" aria-label={closeLabel} onClick={onCloseSidebar} />
      )}
      {sidebar}
      <div className="workspace" inert={sidebarOpen}>
        {children}
      </div>
    </div>
  );
}

export function Sidebar({
  label,
  innerRef,
  children,
}: {
  label: string;
  innerRef?: Ref<HTMLElement>;
  children: ReactNode;
}) {
  return (
    <aside ref={innerRef} className="sidebar" aria-label={label}>
      {children}
    </aside>
  );
}

export function SidebarSection({ label, children }: { label: string; children: ReactNode }) {
  return (
    <section className="sidebar-section">
      <h2 className="sidebar-label">{label}</h2>
      {children}
    </section>
  );
}

export function Topbar({ children }: { children: ReactNode }) {
  return <header className="topbar">{children}</header>;
}

export function PageFrame({
  title,
  detail,
  action,
  children,
}: {
  title: string;
  detail?: string;
  action?: ReactNode;
  children: ReactNode;
}) {
  return (
    <div className="page-scroll">
      <div className="page">
        <header className="page-heading">
          <div>
            <h1>{title}</h1>
            {detail && <p>{detail}</p>}
          </div>
          {action}
        </header>
        {children}
      </div>
    </div>
  );
}

export function Toolbar({ children }: { children: ReactNode }) {
  return <div className="toolbar">{children}</div>;
}

export function Inspector({
  label,
  testId,
  innerRef,
  header,
  footer,
  children,
}: {
  label: string;
  testId?: string;
  innerRef?: Ref<HTMLElement>;
  header: ReactNode;
  footer?: ReactNode;
  children: ReactNode;
}) {
  return (
    <aside ref={innerRef} className="detail-panel" aria-label={label} data-testid={testId}>
      <div className="panel-header">{header}</div>
      {children}
      {footer && <div className="panel-footer">{footer}</div>}
    </aside>
  );
}
