import {spawn} from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';

async function main() {
  // Bounded live-test harness; the C++ runner owns model, channel and durable session effects.
  const [runnerArg, configArg, workspaceArg, stateArg] = process.argv.slice(2);
  if (!stateArg || process.argv.length !== 6) {
    console.error('Usage: node scripts/qq-live.mjs RUNNER CONFIG WORKSPACE PRIVATE_STATE');
    process.exit(2);
  }
  const runner = fs.realpathSync(runnerArg), config = fs.realpathSync(configArg);
  const workspace = fs.realpathSync(workspaceArg), state = fs.realpathSync(stateArg);
  const mode = fs.statSync(state).mode;
  if ((mode & 0o077) !== 0 || fs.statSync(state).uid !== process.getuid())
    throw Error('State must be owner-private');
  const inside = path.relative(workspace, state);
  if (!inside.startsWith(`..${path.sep}`) && inside !== '..')
    throw Error('State must be outside workspace');
  const lockPath = `${state}/gateway-eval.lock`;
  const lock = fs.openSync(lockPath, 'wx', 0o600);
  const snapshot = `${state}/eval-qq-${crypto.randomUUID()}`;
  process.on('exit', () => {
    fs.closeSync(lock);
    fs.unlinkSync(lockPath);
    if (fs.existsSync(snapshot)) fs.unlinkSync(snapshot);
  });
  fs.copyFileSync(runner, snapshot, fs.constants.COPYFILE_EXCL);
  fs.chmodSync(snapshot, 0o700);
  function durable(name, data) {
    const temp = `${name}.${crypto.randomUUID()}.tmp`;
    const fd = fs.openSync(temp, 'wx', 0o600);
    try {
      fs.writeFileSync(fd, data);
      fs.fsyncSync(fd);
    } finally {
      fs.closeSync(fd);
    }
    fs.renameSync(temp, name);
    const dir = fs.openSync(state, 'r');
    try {
      fs.fsyncSync(dir);
    } finally {
      fs.closeSync(dir);
    }
  }
  const binding = JSON.parse(fs.readFileSync(`${state}/qq-credentials.json`));
  if (!binding.user_openid) throw Error('Owner identity missing');
  const report = (text) => console.log(new Date().toISOString(), text);
  async function api(url, body, headers = {}) {
    const response = await fetch(url, {
      method: body ? 'POST' : 'GET',
      headers: {'Content-Type': 'application/json', ...headers},
      body: body ? JSON.stringify(body) : undefined,
      signal: AbortSignal.timeout(10000)
    });
    if (!response.ok) throw Error(`HTTP ${response.status}`);
    return response.json();
  }
  let ws, heartbeat, child, timer, seq = null, acked = true, stopping = false;
  const queue = [];
  function stop() {
    if (stopping) return;
    stopping = true;
    clearInterval(heartbeat);
    clearTimeout(timer);
    ws?.close();
    child?.kill('SIGTERM');
    report('Gateway stopped; private inputs and journals preserved');
  }
  process.on('SIGINT', stop);
  process.on('SIGTERM', stop);
  function processNext() {
    if (child || stopping || !queue.length) return;
    const event = queue.shift();
    durable(`${state}/event.json`, JSON.stringify(event));
    child = spawn(
        snapshot, [config, workspace, state],
        {env: process.env, stdio: ['ignore', 'pipe', 'pipe']});
    child.stdout.on('data', b => process.stdout.write(b));
    child.stderr.on('data', b => process.stderr.write(b));
    child.on('error', () => {
      report('Native runner launch failed');
      stop();
    });
    child.on('exit', code => {
      child = null;
      if (code !== 0) {
        report('Native runner failed; no automatic replay');
        stop();
      } else
        processNext();
    });
  }
  try {
    const auth = await api(
        'https://bots.qq.com/app/getAppAccessToken',
        {appId: binding.app_id, clientSecret: binding.app_secret});
    const gateway = await api(
        'https://api.sgroup.qq.com/gateway/bot', null,
        {Authorization: `QQBot ${auth.access_token}`, 'X-Union-Appid': binding.app_id});
    const url = new URL(gateway.url);
    if (url.protocol !== 'wss:' || url.hostname !== 'api.sgroup.qq.com')
      throw Error('Unexpected gateway');
    ws = new WebSocket(url);
    ws.addEventListener('message', async ({data}) => {
      try {
        const raw = typeof data === 'string' ? data : await data.text();
        if (raw.length > 1024 * 1024) throw Error('Oversized event');
        const event = JSON.parse(raw);
        if (event.s !== undefined) seq = event.s;
        if (event.op === 10) {
          ws.send(JSON.stringify(
              {op: 2, d: {token: `QQBot ${auth.access_token}`, intents: 1 << 25, shard: [0, 1]}}));
          const ms = event.d.heartbeat_interval;
          if (!Number.isInteger(ms) || ms < 1000 || ms > 120000) throw Error('Invalid heartbeat');
          heartbeat = setInterval(() => {
            if (!acked) {
              report('Heartbeat acknowledgement missed');
              stop();
              return;
            }
            acked = false;
            ws.send(JSON.stringify({op: 1, d: seq}));
          }, ms);
        } else if (event.op === 11)
          acked = true;
        else if (event.op === 1)
          ws.send(JSON.stringify({op: 1, d: seq}));
        else if (event.op === 7 || event.op === 9) {
          report(`Gateway requires reconnect (${event.op}); no automatic replay`);
          stop();
        } else if (event.op === 0 && event.t === 'READY')
          report('QQ Gateway READY; owner-only private reception active');
        else if (event.op === 0 && event.t === 'C2C_MESSAGE_CREATE') {
          if (event.d?.author?.user_openid !== binding.user_openid) {
            report('Ignored non-owner message');
            return;
          }
          const name = `${state}/received-${crypto.randomUUID()}.json`;
          durable(name, raw);
          report('Owner message durably captured');
          if (queue.length >= 16) {
            report('Queue limit reached');
            stop();
            return;
          }
          queue.push(event);
          processNext();
        }
      } catch {
        report('Gateway event failed');
        stop();
      }
    });
    ws.addEventListener('error', () => {
      report('Gateway connection failed');
      stop();
    });
    ws.addEventListener('close', event => {
      report(`Gateway closed (${event.code})`);
      stop();
    });
    timer = setTimeout(stop, 15 * 60 * 1000);
  } catch {
    report('Gateway setup failed');
    stop();
    process.exitCode = 1;
  }
}
main().catch(() => {
  console.error('QQ Gateway evaluation setup failed');
  process.exitCode = 2;
});
