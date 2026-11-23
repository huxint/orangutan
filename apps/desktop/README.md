# Orangutan Desktop Workbench

The local web workbench for Orangutan Personal Agent (React 19, TypeScript, Vite).
Includes six views (**Assistant, Tasks, Reminders, Connections, Memory, Settings**), zh/en bilingual copy, light/dark/system themes, and interactive **Forest Companions** (`Oran`, `Scout`, `Moss`, `Fern`, `Pebble`, `Pip`).

## Run With Live C++26 Runtime

Start the local Personal Agent server in one terminal:

```sh
xmake build -j4 orangutan
build/linux/x86_64/release/orangutan serve --demo
# Or with your real provider config:
# build/linux/x86_64/release/orangutan serve --config config.example.json
```

Then start the desktop UI in another terminal (Node.js 24+):

```sh
npm --prefix apps/desktop ci
npm --prefix apps/desktop run dev
```

Open <http://127.0.0.1:5173>. Vite proxies `/api` to `http://127.0.0.1:18790` automatically:

- When `orangutan serve` is running, the workbench runs in **live runtime mode** (real C++26 `AgentSession` streaming, real `FileList`/`FileRead`/`FileWrite`/`ShellExec` tools, live interactive approvals, SQLite memory and SQLite reminders).
- When `orangutan serve` is stopped, the workbench falls back smoothly to **offline demo mode** in your browser.

## Verify

```sh
npm --prefix apps/desktop run build
npm --prefix apps/desktop test
npm --prefix apps/desktop run format:check
```

See [docs/design-docs/desktop-ui.md](../../docs/design-docs/desktop-ui.md) for architecture and state boundaries.
