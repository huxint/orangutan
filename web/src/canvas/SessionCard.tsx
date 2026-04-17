import { useEffect, useRef } from "react";
import { useSubmitMessage, useWorkspace, useWorkspaceState } from "../state/WorkspaceProvider";
import { api } from "../api/client";
import type { ApprovalRequest } from "../api/types";
import { StreamText } from "../pretext/StreamText";
import { cn } from "../lib/utils";
import type { AssistantTurn, SessionState, ToolCallRecord, Turn } from "../state/workspace";

interface Props {
  session: SessionState;
}

export function SessionCard({ session }: Props) {
  const store = useWorkspace();
  const agents = useWorkspaceState((s) => s.agents);
  const agent = agents.find((a) => a.key === session.agentKey) ?? null;
  const focused = useWorkspaceState((s) => s.focusSession) === session.id;
  const { submit, abort, submitting } = useSubmitMessage(session.id);
  const composerRef = useRef<HTMLTextAreaElement | null>(null);
  const scrollRef = useRef<HTMLDivElement | null>(null);

  useEffect(() => {
    if (focused) composerRef.current?.focus();
  }, [focused]);

  useEffect(() => {
    const el = scrollRef.current;
    if (!el) return;
    if (el.scrollHeight - el.scrollTop - el.clientHeight < 240) {
      el.scrollTop = el.scrollHeight;
    }
  }, [session.turns.length, session.turns[session.turns.length - 1]?.role]);

  const onKeyDown = (e: React.KeyboardEvent<HTMLTextAreaElement>) => {
    if (e.key === "Enter" && !e.shiftKey) {
      e.preventDefault();
      void submit(session.composer);
    }
  };

  return (
    <div
      className={cn(
        "surface-elevated rounded-3xl overflow-hidden flex flex-col text-[var(--color-text)] transition-shadow duration-300",
        focused ? "ring-1 ring-[var(--color-accent)]/40 shadow-xl" : "",
      )}
      style={{ width: 540, maxHeight: "72vh" }}
      onClick={() => store.setFocusSession(session.id)}
    >
      <Header session={session} agentKey={session.agentKey} agentLabel={agent?.model ?? ""} onClose={() => store.removeSession(session.id)} />

      <div
        ref={scrollRef}
        className="flex-1 overflow-y-auto no-scrollbar px-6 py-4"
        style={{ minHeight: 120 }}
      >
        {session.turns.length === 0 && (
          <Placeholder agentKey={session.agentKey} />
        )}
        <div className="flex flex-col gap-5">
          {session.turns.map((turn) => (
            <TurnView key={turn.id} turn={turn} />
          ))}
        </div>
      </div>

      {session.pendingApproval && (
        <ApprovalBar
          approval={session.pendingApproval}
          onDecide={async (approved) => {
            if (!session.id.startsWith("draft-")) {
              await api.submitApproval(
                session.id,
                session.pendingApproval!.request_id,
                approved,
              );
            }
            store.updateSession(session.id, (s) => ({
              ...s,
              pendingApproval: null,
            }));
          }}
        />
      )}

      <div className="relative px-5 pt-3 pb-5 border-t border-[var(--color-line)]">
        <textarea
          ref={composerRef}
          rows={1}
          placeholder={session.streaming ? "streaming…" : "ask, instruct, probe —"}
          disabled={session.streaming}
          value={session.composer}
          onChange={(e) =>
            store.updateSession(session.id, (s) => ({
              ...s,
              composer: e.target.value,
            }))
          }
          onKeyDown={onKeyDown}
          className="w-full bg-transparent resize-none outline-none font-serif text-[17px] leading-snug placeholder:text-[var(--color-text-faint)]"
          style={{ fontVariationSettings: '"opsz" 24' }}
        />
        <div className="flex items-center justify-between mt-3 text-[11px] uppercase tracking-[0.22em] text-[var(--color-text-faint)] font-mono">
          <span>
            ⏎ send · ⇧⏎ newline · ⌘K palette
          </span>
          {session.streaming ? (
            <button
              onClick={abort}
              className="chip"
              data-state="error"
            >
              stop
            </button>
          ) : (
            <button
              onClick={() => void submit(session.composer)}
              disabled={submitting || !session.composer.trim()}
              className={cn(
                "chip transition-opacity",
                !session.composer.trim() && "opacity-40",
              )}
              data-state={session.composer.trim() ? "active" : ""}
            >
              send →
            </button>
          )}
        </div>
      </div>
    </div>
  );
}

function Header({
  session,
  agentKey,
  agentLabel,
  onClose,
}: {
  session: SessionState;
  agentKey: string;
  agentLabel: string;
  onClose: () => void;
}) {
  return (
    <div className="flex items-start justify-between px-6 pt-5 pb-3 border-b border-[var(--color-line)]">
      <div>
        <div className="text-[10px] uppercase tracking-[0.22em] text-[var(--color-text-faint)] font-mono">
          session · {agentKey}
        </div>
        <div className="title-display text-[22px] mt-1">
          {session.id.startsWith("draft-") ? "untitled" : session.id.slice(0, 16)}
        </div>
        <div className="text-[11px] font-mono text-[var(--color-text-dim)] mt-1">
          {agentLabel}
        </div>
      </div>
      <button
        onClick={(e) => {
          e.stopPropagation();
          onClose();
        }}
        className="chip"
      >
        ×
      </button>
    </div>
  );
}

function Placeholder({ agentKey }: { agentKey: string }) {
  return (
    <div className="text-[var(--color-text-faint)] font-serif text-[22px] leading-tight mt-2">
      <span className="text-[var(--color-text-dim)]">{agentKey}</span>{" "}
      is listening. Begin anywhere.
    </div>
  );
}

function TurnView({ turn }: { turn: Turn }) {
  if (turn.role === "user") {
    return (
      <div className="text-right">
        <div className="inline-block max-w-[88%] text-left rounded-2xl px-4 py-2 bg-[var(--color-accent-soft)] text-[var(--color-text)] font-serif text-[17px]"
             style={{ fontVariationSettings: '"opsz" 20' }}>
          {turn.text}
        </div>
      </div>
    );
  }
  return <AssistantView turn={turn} />;
}

function AssistantView({ turn }: { turn: AssistantTurn }) {
  const text = turn.segments
    .map((s) => (s.kind === "text" ? s.text : s.kind === "thinking" ? "" : ""))
    .join("");
  const thinking = turn.segments
    .filter((s) => s.kind === "thinking")
    .map((s) => (s.kind === "thinking" ? s.text : ""))
    .join("");
  const toolCalls = [...turn.toolCalls.values()];

  return (
    <div className="flex flex-col gap-3">
      {thinking && (
        <details className="text-[13px] text-[var(--color-text-dim)] font-mono">
          <summary className="cursor-pointer uppercase tracking-[0.22em] text-[10px]">
            thinking
          </summary>
          <div className="mt-2 whitespace-pre-wrap opacity-80 leading-relaxed">
            {thinking}
          </div>
        </details>
      )}
      {text && (
        <StreamText text={text} maxWidth={470} fontSize={16} streaming={turn.streaming} />
      )}
      {turn.streaming && !text && <TypingIndicator />}
      {toolCalls.length > 0 && (
        <div className="flex flex-col gap-2 pt-1">
          {toolCalls.map((call) => (
            <ToolPanel key={call.id} call={call} />
          ))}
        </div>
      )}
      {turn.error && (
        <div className="text-[13px] text-[var(--color-warn)] font-mono">
          error · {turn.error}
        </div>
      )}
    </div>
  );
}

function TypingIndicator() {
  return (
    <div className="flex items-center gap-1 text-[var(--color-accent)]">
      <span className="w-1.5 h-1.5 rounded-full bg-current anim-pulse-dot" />
      <span className="w-1.5 h-1.5 rounded-full bg-current anim-pulse-dot" style={{ animationDelay: "150ms" }} />
      <span className="w-1.5 h-1.5 rounded-full bg-current anim-pulse-dot" style={{ animationDelay: "300ms" }} />
    </div>
  );
}

function ToolPanel({ call }: { call: ToolCallRecord }) {
  const streaming = call.output === undefined;
  const state = streaming ? "active" : call.isError ? "error" : "";
  return (
    <details
      className={cn(
        "rounded-xl border border-[var(--color-line)] bg-[var(--color-bg-1)]/60 overflow-hidden",
      )}
    >
      <summary className="cursor-pointer select-none flex items-center gap-2 px-3 py-2 text-[12px] font-mono">
        <span className="chip" data-state={state}>{call.name}</span>
        <span className="text-[var(--color-text-faint)] truncate flex-1">
          {summarizeInput(call.input)}
        </span>
        {streaming ? (
          <span className="text-[var(--color-accent)] text-[10px] uppercase tracking-[0.22em]">
            running
          </span>
        ) : (
          <span
            className={cn(
              "text-[10px] uppercase tracking-[0.22em]",
              call.isError ? "text-[var(--color-warn)]" : "text-[var(--color-life)]",
            )}
          >
            {call.isError ? "error" : "ok"}
          </span>
        )}
      </summary>
      <div className="px-3 pb-3 pt-1 text-[12px] font-mono leading-relaxed text-[var(--color-text-dim)] whitespace-pre-wrap max-h-72 overflow-y-auto">
        {streaming ? <span className="caret">streaming</span> : call.output}
      </div>
    </details>
  );
}

function summarizeInput(input: unknown): string {
  if (input === null || input === undefined) return "";
  if (typeof input === "string") return input;
  try {
    const json = JSON.stringify(input);
    if (json.length > 140) return `${json.slice(0, 137)}…`;
    return json;
  } catch {
    return String(input);
  }
}

function ApprovalBar({
  approval,
  onDecide,
}: {
  approval: ApprovalRequest;
  onDecide: (approved: boolean) => Promise<void>;
}) {
  return (
    <div className="px-5 py-4 border-t border-[var(--color-line)] bg-[var(--color-accent-soft)]">
      <div className="text-[10px] uppercase tracking-[0.22em] text-[var(--color-accent)] font-mono">
        approval required · {approval.tool}
      </div>
      <div className="font-serif text-[16px] mt-1">{approval.prompt}</div>
      {approval.command && (
        <pre className="font-mono text-[12px] bg-[var(--color-bg-2)] border border-[var(--color-line)] rounded-lg p-2 mt-2 overflow-x-auto">
          {approval.command}
        </pre>
      )}
      <div className="flex gap-2 mt-3">
        <button
          onClick={() => void onDecide(true)}
          className="chip"
          data-state="active"
        >
          approve
        </button>
        <button
          onClick={() => void onDecide(false)}
          className="chip"
          data-state="error"
        >
          deny
        </button>
      </div>
    </div>
  );
}
