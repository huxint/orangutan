# Tests And Benchmarks

Every library has a `tests/<lib>` Catch2 bucket and a `bench/<lib>` nanobench
bucket. Xmake names them `test-<lib>` and `bench-<lib>`.

Test public behavior: successful results, errors, authorization, limits,
state transitions, cancellation and storage integrity. Use controlled providers
and transports while retaining the meaningful runtime logic. Tests of deleted
application behavior leave with their callers; retain shared-core regressions.

Async tests use a real executor, explicit synchronization and a hard timeout.
Assign awaited results to locals before Catch assertions. Keep independent
scenarios separate. New or changed tests must fail on the defect they guard.

Run affected build/tests during a slice and `make ci` before a commit. Broad
runtime refactors also run the complete release suite. Debug ASan/UBSan checks
are required for lifetime work. [CICD](../CICD.md) owns hosted gate definitions.

Use existing build and test targets in the working tree for routine verification.
Reusable checks belong in `tests/` or `scripts/`. Separate validation workspaces,
fault-injection campaigns, custom scripts and retained reports are not required
for each change. Additional diagnostics address a concrete failure or an explicit
investigation; clean up their temporary files when finished.

Bench when a meaningful design or performance choice needs evidence. Compare
reasonable alternatives under the same workload; prefer the simpler solution
when measurements do not justify complexity. Report compiler, mode and hardware
limits. [compile-budget](compile-budget.md) owns compile-time thresholds.

```sh
xmake build test-tool
xmake run test-tool
xmake build bench-tool
xmake run bench-tool
```

## Working-Context Evaluation

`eval-context` is an opt-in executable, separate from `xmake test`. It uses
synthetic task transcripts to compare full-history baselines with compacted
sessions, then closes/reopens their databases and probes again after more context
pressure. Cases cover goals/constraints, superseded decisions, completed versus
pending work, and a model-directed read of a large fixed diagnostic tool result.
The tool has no project filesystem or subprocess effects. Each run uses a new
private output directory, independent identities and disabled long-term memory.

```sh
xmake build -j4 eval-context
build/linux/x86_64/release/eval-context --self-test --output /tmp/oran-context-check
build/linux/x86_64/release/eval-context --config eval/context/deepseek.example.json --output /tmp/oran-context-live --repeat 2
```

`--self-test` exercises plumbing with a provider that knows the fixture answers;
it is explicitly labelled `controlled_plumbing_check`, not model evidence. Live
runs require an explicit provider config and its named credential environment
variable. The DeepSeek example uses `ORAN_CONTEXT_EVAL_KEY` and the documented
[Anthropic endpoint](https://api-docs.deepseek.com/guides/anthropic_api). It carries
peak USD [pricing](https://api-docs.deepseek.com/quick_start/pricing) observed on
2026-09-28; refresh rates for current billing. Cost values are configured estimates,
not invoices, and remain null when pricing is unavailable.

The runner accepts `--case`, `--repeat` (1–5), `--max-calls` (1–512),
`--context-tokens` (8192–32768), and `--deadline-seconds` (1–3600). Defaults are all
cases, one repeat, 192 attempts, a 16384-token context budget, 2048 summary bytes,
and a 900-second overall deadline. HTTP requests time out after 60 seconds;
SIGINT/SIGTERM and the deadline request cancellation and await runtime cleanup.
The 262144-token baseline budget avoids compaction for these bounded fixtures;
select a deployment model that can accept their full history. Token counting uses
the runtime's conservative byte estimator, so this is synthetic context pressure,
not a calibration of the model's physical window.

`report.json` records each phase's exact-field answer checks, actual checkpoint
coverage/revisions, tool calls, provider attempts, served models, input/output/cache
tokens, estimated cost and elapsed time. Per-call metadata records summary versus
answer, stop reason, text/thinking byte counts and output tokens, without response
bodies; local context-budget failures include their runtime diagnostic. Arrays grade independent of order; wrong
labels, missing constraints, obsolete decisions and invented completions fail.
Compacted phases must actually advance checkpoints (at least twice in the first
phase); baseline phases must remain uncompacted. A paired regression means a
baseline phase passed and its compacted counterpart failed. Provider errors and
incomplete runs remain visible rather than counting as successful retention.
Account/authentication failures (HTTP 401/402/403) stop further calls. Reports
include the numeric HTTP status but no upstream error body. Operational failures
are counted separately and never scored as malformed model answers.
Original fixture/transcript rows and traces remain in each case's private database
for inspection. Raw provider errors, headers and credential values are not put in
the report. Exit codes are 0 for a complete pass, 1 for failed/incomplete results,
and 2 for invalid setup.

These fixed tasks isolate retention and status errors; they do not establish
arbitrary coding-task success or spontaneous long-term-memory behavior. A strict
field mismatch can be a paraphrase rather than forgotten information: inspect
`grading.answer` and individual checks. Extra inspection logs in completed/pending
fields are task-state contamination, while `false_completion` specifically means
that an explicitly pending task label was reported as completed. Repeat
with the deployment model and inspect failed answers and checkpoints before
changing memory policy. Grader negatives, budget exhaustion and controlled
end-to-end execution are covered by `test-bootstrap`.

## QQ Live Dialogue Evaluation

`eval-qq` processes one authenticated QQ event supplied by a trusted local ingress
harness. It is opt-in and does not connect a Gateway or start public ingress.
The current Tencent `qqbot-nodejs` SDK supports a WebSocket Gateway; a live
experiment can authenticate there and feed C2C events to this runner without
adding a WebSocket dependency to the runtime. The bootstrap webhook API remains
independent of this ingress choice.

```sh
xmake build -j4 eval-qq
python3 tests/eval/test_qq.py build/linux/x86_64/release/eval-qq
build/linux/x86_64/release/eval-qq "$CONFIG" "$WORKSPACE" "$QQ_STATE_DIR"
```

The private state directory must already contain `qq-credentials.json` from
`oran-qq-login` and the trusted harness's `event.json` (at most 1 MiB). The event
file is not independently authenticated; never feed untrusted local files or
public HTTP bodies to this entry point. Only direct text/images from the saved
scanning user are admitted. Missing owner identity fails closed. State must be outside the
workspace; extra filesystem roots are refused. Provider credentials use the
configuration's ordinary environment references. `eval/qq/deepseek.example.json`
uses `ORAN_QQ_MODEL_KEY` and the existing DeepSeek Anthropic-compatible route.
The runner enables joined QQ typing status and native Markdown; `--no-typing`
and `--plain-text` explicitly disable them. Child delegation is disabled;
model/tool permissions still come from the supplied configuration.

`live-journal.json` binds the app, user, workspace and persistent session ID.
It saves input before the model runs, answer before sending, and each confirmed
receipt. A failed or ambiguous operation leaves `pending` intact and blocks the
next invocation. Preserve and inspect it before any manual reconciliation;
there is no automatic retry or acknowledgment command. Delivered event IDs
suppress duplicates. The journal is limited to 4 MiB on both read and write;
reaching that bound stops the evaluator without evicting deliveries. Private
`live-sessions.db`, `live-memory.db` and
`live-audit.db` preserve runtime history independently of the delivery journal.
Do not delete user records to restart an evaluation.

SIGINT/SIGTERM requests cancellation and joins active work. The evaluation runner
is a bounded local test entry point, not a durable production Gateway service.
The CLI regression check covers foreign/missing owner, binding changes, pending
preservation, duplicate suppression, the journal byte bound and denied-provider recovery.
Live acceptance additionally requires user-originated QQ messages, real provider
responses, remote send receipts and confirmation in the user's QQ client. A
correct answer after a second process opens the same session verifies that
specific continuation case, not general long-term-memory quality.

The QQ evaluator resolves quoted incoming messages and confirmed outgoing chunks
from this conversation's journal, including quoted images. Authenticated inline
`msg_elements.content` is also preserved, so a quote can work before a local
reference index exists. Unknown references
are marked unavailable in the typed prompt. Images use credential-free HTTPS
requests only to `multimedia.nt.qq.com.cn`, `gchat.qpic.cn` or `c2cpicdw.qpic.cn`;
redirects are disabled. `ChannelAttachment` authorizes each request. Requests are
bounded to 20 seconds and 5 MiB, and PNG/JPEG/GIF/WebP signatures are checked before
base64 encoding. One current and one referenced image can enter the prompt.
Download failures return a useful resend instruction. Model visual capability
is still required; transport support alone does not establish image understanding.
The Telegram and QQ hosts share the same image byte/signature conversion.

Native Markdown splitting preserves UTF-8, closes/reopens ordinary three-character
fenced code blocks and preflights QQ's five-part limit. Overlong answers receive
a shorter-request suggestion; the successful model transcript remains preserved.
`/help` (`/start`), `/status` and `/new` are model-free and journaled like ordinary
replies. Status shows the configured model, stored message count and session ID.
New-session identity and its confirmation persist together; old transcripts and
conversation-scoped memory remain. Commands in quoted messages or image captions
do not execute. Unexpected standalone commands/arguments return help.

For an interactive local Gateway experiment, Node 22+ supplies the built-in
WebSocket and fetch APIs; no npm dependency is installed:

```sh
node scripts/qq-live.mjs build/linux/x86_64/release/eval-qq "$CONFIG" "$WORKSPACE" "$QQ_STATE_DIR"
```

This harness authenticates the saved bot, subscribes to direct messages from the
scanning user and fsyncs each received envelope before invoking the native runner.
It holds an exclusive `gateway-eval.lock` and snapshots the runner executable so
a rebuild cannot interrupt message startup. It runs for at most 15 minutes, with
at most 16 queued messages; the native journal byte bound still applies. Heartbeat failure,
reconnect requests or runner failure stop the experiment without automatic replay.
Private received envelopes remain for inspection. A stale lock after a crash
requires operator inspection before removal. It is an evaluation harness, not
a production reconnecting Gateway service.

Reference behavior follows Tencent's [QQ plugin feature catalogue](https://github.com/tencent-connect/openclaw-qqbot#readme)
and [`qqbot-nodejs` protocol implementation](https://www.npmjs.com/package/@tencent-connect/qqbot-nodejs),
plus the Telegram command/input lifecycle implemented in this repository.

The QQ overlay explicitly explains that attached images are native visual input,
not files requiring another tool. It asks for visible details and calibrated
uncertainty. This follows the visual-content framing in the reviewed
[Claude Code image-read prompt](https://github.com/Piebald-AI/claude-code-system-prompts/blob/main/system-prompts/tool-description-readfile-compact.md),
adapted to already-attached chat images rather than claiming a nonexistent tool.
