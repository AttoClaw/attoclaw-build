#!/usr/bin/env python3
import argparse
import json
import os
import shutil
import signal
import subprocess
import threading
import time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

HTML = """<!doctype html>
<html lang=\"en\">
<head>
  <meta charset=\"utf-8\" />
  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\" />
  <title>AttoClaw Dashboard</title>
  <link rel=\"preconnect\" href=\"https://fonts.googleapis.com\">
  <link rel=\"preconnect\" href=\"https://fonts.gstatic.com\" crossorigin>
  <link href=\"https://fonts.googleapis.com/css2?family=Space+Grotesk:wght@400;500;700&family=JetBrains+Mono:wght@400;600&display=swap\" rel=\"stylesheet\">
  <style>
    :root {
      --bg-0: #0a0d14;
      --bg-1: #111827;
      --bg-2: #1f2937;
      --card: #0f172a;
      --line: #2c3a55;
      --txt: #dbe7ff;
      --muted: #8ea4cc;
      --ok: #22c55e;
      --warn: #f59e0b;
      --bad: #ef4444;
      --accent: #0ea5e9;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      font-family: \"Space Grotesk\", sans-serif;
      color: var(--txt);
      background:
        radial-gradient(1200px 500px at 12% -12%, #12355f 0%, transparent 65%),
        radial-gradient(700px 400px at 95% 8%, #074f57 0%, transparent 60%),
        linear-gradient(180deg, var(--bg-0), var(--bg-1));
      min-height: 100vh;
    }
    .shell {
      max-width: 1240px;
      margin: 0 auto;
      padding: 20px;
    }
    .top {
      display: flex;
      justify-content: space-between;
      align-items: center;
      gap: 12px;
      margin-bottom: 16px;
    }
    .title {
      font-size: 1.3rem;
      letter-spacing: 0.04em;
      font-weight: 700;
    }
    .sub { color: var(--muted); font-size: 0.9rem; }
    .btn {
      border: 1px solid var(--line);
      background: linear-gradient(180deg, #1f2d46, #172236);
      color: var(--txt);
      border-radius: 10px;
      padding: 8px 12px;
      cursor: pointer;
      font-weight: 600;
    }
    .btn:hover { border-color: #4a678f; }
    .grid {
      display: grid;
      grid-template-columns: repeat(12, 1fr);
      gap: 12px;
    }
    .card {
      background: linear-gradient(160deg, rgba(17,24,39,.95), rgba(15,23,42,.93));
      border: 1px solid var(--line);
      border-radius: 14px;
      padding: 12px;
      box-shadow: 0 6px 28px rgba(0,0,0,.25);
    }
    .card h3 {
      margin: 0 0 8px;
      font-size: 0.9rem;
      text-transform: uppercase;
      letter-spacing: 0.08em;
      color: #99b7e6;
    }
    .span-3 { grid-column: span 3; }
    .span-4 { grid-column: span 4; }
    .span-5 { grid-column: span 5; }
    .span-6 { grid-column: span 6; }
    .span-7 { grid-column: span 7; }
    .span-8 { grid-column: span 8; }
    .span-12 { grid-column: span 12; }
    .mono {
      font-family: \"JetBrains Mono\", monospace;
      white-space: pre-wrap;
      font-size: 12px;
      line-height: 1.45;
      max-height: 330px;
      overflow: auto;
      background: rgba(9, 14, 24, 0.65);
      border: 1px solid #22314f;
      border-radius: 10px;
      padding: 10px;
    }
    .kv { display: flex; justify-content: space-between; padding: 4px 0; border-bottom: 1px dashed #22314f; }
    .kv:last-child { border-bottom: 0; }
    input, textarea, select {
      width: 100%;
      border: 1px solid #2d4266;
      border-radius: 9px;
      background: #0a1426;
      color: var(--txt);
      padding: 9px;
      font: inherit;
      margin: 4px 0 8px;
    }
    textarea { min-height: 96px; resize: vertical; }
    .row { display: flex; gap: 8px; flex-wrap: wrap; align-items: center; }
	    .chip {
	      display: inline-block;
	      font-size: 11px;
	      padding: 3px 8px;
	      border: 1px solid #355381;
	      border-radius: 999px;
	      color: #bcd2fb;
	    }
	    .split {
	      display: grid;
	      grid-template-columns: 1fr 1fr;
	      gap: 12px;
	      align-items: start;
	    }
	    .pane {
	      border: 1px solid #22314f;
	      border-radius: 12px;
	      background: rgba(9, 14, 24, 0.45);
	      padding: 10px;
	      overflow: hidden;
	    }
	    .tree details { border: 1px solid #22314f; border-radius: 10px; padding: 8px; margin: 8px 0; }
	    .tree summary { cursor: pointer; color: #bcd2fb; font-weight: 700; letter-spacing: 0.02em; }
	    .tree .field { margin: 8px 0; }
	    .tree .key { font-family: "JetBrains Mono", monospace; font-size: 12px; color: #9fb8e8; }
	    .tree .hint { font-size: 12px; color: var(--muted); }
	    @media (max-width: 920px) {
	      .span-3, .span-4, .span-5, .span-6, .span-7, .span-8 { grid-column: span 12; }
	      .split { grid-template-columns: 1fr; }
	    }
	  </style>
</head>
<body>
  <div class=\"shell\">
    <div class=\"top\">
      <div>
        <div class=\"title\">AttoClaw Control Dashboard</div>
        <div class=\"sub\" id=\"meta\"></div>
      </div>
      <div class=\"row\">
        <button class=\"btn\" onclick=\"refreshAll()\">Refresh</button>
        <button class=\"btn\" onclick=\"runOnboard()\">Run Onboard</button>
      </div>
    </div>

    <div class=\"grid\">
      <div class=\"card span-4\">
        <h3>Status</h3>
        <div id=\"statusSummary\"></div>
      </div>

      <div class=\"card span-4\">
        <h3>Gateway</h3>
        <div class=\"row\">
          <button class=\"btn\" onclick=\"gatewayStart()\">Start</button>
          <button class=\"btn\" onclick=\"gatewayStop()\">Stop</button>
          <span class=\"chip\" id=\"gatewayState\">unknown</span>
        </div>
        <div id=\"gatewayInfo\" class=\"sub\" style=\"margin-top:8px\"></div>
      </div>

      <div class=\"card span-4\">
        <h3>Channels</h3>
        <div id=\"channelsSummary\" class=\"mono\"></div>
      </div>

      <div class=\"card span-7\">
        <h3>Agent</h3>
        <textarea id=\"agentPrompt\" placeholder=\"Enter prompt...\"></textarea>
        <div class=\"row\">
          <input id=\"agentSession\" placeholder=\"session (optional, e.g. cli:dashboard)\" style=\"max-width:280px\" />
          <label><input type=\"checkbox\" id=\"agentVision\" /> vision</label>
          <label><input type=\"checkbox\" id=\"agentCodex\" /> codex</label>
          <label><input type=\"checkbox\" id=\"agentGemini\" /> gemini</label>
          <button class=\"btn\" onclick=\"sendPrompt()\">Send</button>
        </div>
        <div id=\"agentOut\" class=\"mono\"></div>
      </div>

      <div class=\"card span-5\">
        <h3>Cron</h3>
        <input id=\"cronName\" placeholder=\"Job name\" />
        <textarea id=\"cronMsg\" placeholder=\"Message\"></textarea>
        <input id=\"cronEvery\" placeholder=\"every seconds (optional)\" />
        <input id=\"cronExpr\" placeholder=\"cron expr (optional)\" />
        <input id=\"cronAt\" placeholder=\"at ISO (optional)\" />
        <div class=\"row\">
          <button class=\"btn\" onclick=\"cronAdd()\">Add</button>
          <input id=\"cronRemoveId\" placeholder=\"job id\" style=\"max-width:180px\" />
          <button class=\"btn\" onclick=\"cronRemove()\">Remove</button>
        </div>
      </div>

      <div class=\"card span-6\">
        <h3>Cron List</h3>
        <div id=\"cronList\" class=\"mono\"></div>
      </div>

	      <div class=\"card span-6\">
	        <h3>Gateway Log</h3>
	        <div id=\"gatewayLog\" class=\"mono\"></div>
	      </div>

      <div class=\"card span-4\">
        <h3>Send Test</h3>
        <select id=\"sendChannel\">
          <option value=\"telegram\">telegram</option>
          <option value=\"whatsapp\">whatsapp</option>
          <option value=\"slack\">slack</option>
          <option value=\"discord\">discord</option>
          <option value=\"email\">email</option>
        </select>
        <input id=\"sendTo\" placeholder=\"to (chat_id/channel_id/email)\" />
        <textarea id=\"sendMsg\" placeholder=\"message\"></textarea>
        <div class=\"row\">
          <button class=\"btn\" onclick=\"sendTest()\">Send</button>
        </div>
        <div id=\"sendOut\" class=\"mono\"></div>
      </div>

      <div class=\"card span-4\">
        <h3>Doctor</h3>
        <div class=\"row\">
          <button class=\"btn\" onclick=\"refreshDoctor()\">Run</button>
        </div>
        <div id=\"doctorOut\" class=\"mono\"></div>
      </div>

      <div class=\"card span-4\">
        <h3>Metrics</h3>
        <div class=\"row\">
          <button class=\"btn\" onclick=\"refreshMetrics()\">Refresh</button>
        </div>
        <div id=\"metricsOut\" class=\"mono\"></div>
      </div>

	      <div class=\"card span-12\">
	        <h3>Config</h3>
	        <div class=\"row\" style=\"margin-bottom:8px\">
	          <button class=\"btn\" onclick=\"configReload()\">Reload</button>
	          <button class=\"btn\" onclick=\"configRevealRaw()\">Reveal Raw</button>
	          <button class=\"btn\" onclick=\"configApplyRaw()\">Apply Raw</button>
	          <button class=\"btn\" onclick=\"configSave()\">Save & Restart</button>
	          <span class=\"chip\" id=\"configPath\">~/.attoclaw/config.json</span>
	          <span class=\"chip\" id=\"configState\">idle</span>
	        </div>
	        <div class=\"split\">
	          <div class=\"pane tree\">
	            <div class=\"sub\" style=\"margin-bottom:8px\">Structured editor (booleans are toggleable).</div>
	            <div id=\"configTree\"></div>
	          </div>
	          <div class=\"pane\">
	            <div class=\"sub\" style=\"margin-bottom:8px\">Raw JSON editor (advanced).</div>
	            <textarea id=\"configRaw\" style=\"min-height:360px\"></textarea>
	          </div>
	        </div>
	      </div>

	      <div class=\"card span-12\">
	        <h3>Command Output</h3>
	        <div id=\"opsOut\" class=\"mono\"></div>
	      </div>
    </div>
  </div>

  <script>
    async function api(path, method='GET', body=null) {
      const res = await fetch(path, {
        method,
        headers: {'Content-Type': 'application/json'},
        body: body ? JSON.stringify(body) : null
      });
      return res.json();
    }

    function setText(id, text) {
      document.getElementById(id).textContent = text || '';
    }

    function appendOps(text) {
      const el = document.getElementById('opsOut');
      const ts = new Date().toLocaleTimeString();
      el.textContent = `[${ts}] ${text}\n\n` + el.textContent;
    }

	    async function refreshAll() {
	      const sum = await api('/api/summary');
	      setText('meta', `bin: ${sum.bin} | host: ${location.host}`);
	      setText('statusSummary', sum.status || '');
	      setText('channelsSummary', sum.channels || '');
	      setText('cronList', sum.cron || '');
	      setText('gatewayState', sum.gateway.running ? 'running' : 'stopped');
	      setText('gatewayInfo', sum.gateway.running ? `pid ${sum.gateway.pid}` : 'not running');
	      await refreshGatewayLog();
	      await refreshMetrics();
	    }

	    async function refreshGatewayLog() {
	      const data = await api('/api/gateway/log');
	      setText('gatewayLog', (data.lines || []).join('\n'));
	    }

	    async function refreshDoctor() {
	      const out = await api('/api/doctor');
	      setText('doctorOut', out.output || out.error || '');
	    }

	    async function refreshMetrics() {
	      const out = await api('/api/metrics');
	      setText('metricsOut', JSON.stringify(out.metrics || {}, null, 2));
	    }

	    async function sendTest() {
	      const channel = document.getElementById('sendChannel').value;
	      const to = (document.getElementById('sendTo').value || '').trim();
	      const message = (document.getElementById('sendMsg').value || '').trim();
	      const out = await api('/api/send', 'POST', {channel, to, message});
	      setText('sendOut', out.output || out.error || '');
	      appendOps(`send: ${out.ok ? 'ok' : 'failed'}`);
	    }

	    let configWorking = null;
	    let configRawRevealed = false;

	    function setConfigState(text) { setText('configState', text); }

	    function isPlainObject(v) { return v && typeof v === 'object' && !Array.isArray(v); }

	    function deepClone(obj) { return JSON.parse(JSON.stringify(obj)); }

	    function detectKind(v) {
	      if (v === null) return 'null';
	      if (Array.isArray(v)) return 'array';
	      return typeof v;
	    }

	    function isSecretKey(path) {
	      const p = (path || '').toLowerCase();
	      return p.includes('apikey') || p.includes('token') || p.includes('password') || p.includes('bridgetoken');
	    }

	    function setByPath(obj, path, value) {
	      const parts = path.split('.');
	      let cur = obj;
	      for (let i = 0; i < parts.length - 1; i++) {
	        const p = parts[i];
	        if (!isPlainObject(cur[p])) cur[p] = {};
	        cur = cur[p];
	      }
	      cur[parts[parts.length - 1]] = value;
	    }

	    function syncRawFromWorking() {
	      if (!configRawRevealed) return;
	      document.getElementById('configRaw').value = JSON.stringify(configWorking, null, 2);
	    }

	    function syncWorkingFromRaw() {
	      const raw = document.getElementById('configRaw').value;
	      const obj = JSON.parse(raw);
	      if (!isPlainObject(obj)) throw new Error('Config must be a JSON object');
	      configWorking = obj;
	    }

	    function renderValueEditor(path, key, value) {
	      const kind = detectKind(value);
	      const wrap = document.createElement('div');
	      wrap.className = 'field';

	      const label = document.createElement('div');
	      label.className = 'key';
	      label.textContent = key;
	      wrap.appendChild(label);

	      if (kind === 'boolean') {
	        const row = document.createElement('div');
	        row.className = 'row';
	        const cb = document.createElement('input');
	        cb.type = 'checkbox';
	        cb.checked = !!value;
	        const hint = document.createElement('span');
	        hint.className = 'hint';
	        hint.textContent = cb.checked ? 'true' : 'false';
	        cb.onchange = () => {
	          setByPath(configWorking, path, cb.checked);
	          hint.textContent = cb.checked ? 'true' : 'false';
	          syncRawFromWorking();
	          setConfigState('dirty');
	        };
	        row.appendChild(cb);
	        row.appendChild(hint);
	        wrap.appendChild(row);
	        return wrap;
	      }

	      if (kind === 'number') {
	        const input = document.createElement('input');
	        input.type = 'number';
	        input.value = String(value);
	        input.onchange = () => {
	          const n = Number(input.value);
	          if (!Number.isFinite(n)) return;
	          setByPath(configWorking, path, n);
	          syncRawFromWorking();
	          setConfigState('dirty');
	        };
	        wrap.appendChild(input);
	        return wrap;
	      }

	      if (kind === 'string') {
	        const input = document.createElement('input');
	        input.type = isSecretKey(path) ? 'password' : 'text';
	        input.value = value;
	        input.onchange = () => {
	          setByPath(configWorking, path, input.value);
	          syncRawFromWorking();
	          setConfigState('dirty');
	        };
	        wrap.appendChild(input);
	        return wrap;
	      }

	      if (kind === 'array') {
	        const allStr = value.every(x => typeof x === 'string');
	        const ta = document.createElement('textarea');
	        ta.value = allStr ? value.join('\n') : JSON.stringify(value, null, 2);
	        ta.onchange = () => {
	          try {
	            const next = allStr ? ta.value.split(/\r?\n/).map(s => s.trim()).filter(Boolean) : JSON.parse(ta.value);
	            setByPath(configWorking, path, next);
	            syncRawFromWorking();
	            setConfigState('dirty');
	          } catch (e) {
	            appendOps('config: invalid array JSON');
	          }
	        };
	        wrap.appendChild(ta);
	        return wrap;
	      }

	      const ta = document.createElement('textarea');
	      ta.value = JSON.stringify(value, null, 2);
	      ta.onchange = () => {
	        try {
	          const next = JSON.parse(ta.value);
	          setByPath(configWorking, path, next);
	          syncRawFromWorking();
	          setConfigState('dirty');
	        } catch (e) {
	          appendOps('config: invalid JSON');
	        }
	      };
	      wrap.appendChild(ta);
	      return wrap;
	    }

	    function renderTree(node, basePath, title) {
	      if (!isPlainObject(node)) {
	        const leaf = document.createElement('div');
	        leaf.appendChild(renderValueEditor(basePath, title || basePath, node));
	        return leaf;
	      }

	      const details = document.createElement('details');
	      details.open = true;
	      const summary = document.createElement('summary');
	      summary.textContent = title || (basePath || 'root');
	      details.appendChild(summary);

	      const keys = Object.keys(node).sort();
	      for (const k of keys) {
	        const v = node[k];
	        const p = basePath ? `${basePath}.${k}` : k;
	        if (isPlainObject(v)) {
	          details.appendChild(renderTree(v, p, k));
	        } else {
	          details.appendChild(renderValueEditor(p, k, v));
	        }
	      }
	      return details;
	    }

	    async function configReload() {
	      setConfigState('loading');
	      const raw = await api('/api/config?raw=1');
	      if (!raw.ok) {
	        appendOps(raw.error || 'config load failed');
	        setConfigState('error');
	        return;
	      }
	      setText('configPath', raw.path);
	      configWorking = deepClone(raw.config || {});
	      configRawRevealed = false;
	      document.getElementById('configRaw').value = 'Secrets hidden. Click "Reveal Raw" to view/edit raw JSON.';

	      const tree = document.getElementById('configTree');
	      tree.innerHTML = '';
	      tree.appendChild(renderTree(configWorking, '', 'config'));

	      setConfigState(raw.exists ? 'loaded' : 'missing');
	    }

	    async function configRevealRaw() {
	      if (!configWorking) return;
	      document.getElementById('configRaw').value = JSON.stringify(configWorking, null, 2);
	      configRawRevealed = true;
	      setConfigState('raw');
	    }

	    async function configApplyRaw() {
	      if (!configRawRevealed) {
	        appendOps('config: reveal raw first');
	        return;
	      }
	      try {
	        syncWorkingFromRaw();
	      } catch (e) {
	        appendOps('config: invalid JSON in raw editor');
	        setConfigState('error');
	        return;
	      }
	      const tree = document.getElementById('configTree');
	      tree.innerHTML = '';
	      tree.appendChild(renderTree(configWorking, '', 'config'));
	      setConfigState('dirty');
	    }

	    async function configSave() {
	      setConfigState('saving');
	      const out = await api('/api/config', 'POST', {config: configWorking});
	      if (!out.ok) {
	        appendOps(out.error || 'config save failed');
	        setConfigState('error');
	        return;
	      }
	      appendOps(out.output || 'config saved');
	      await configReload();
	      await refreshAll();
	    }

	    async function sendPrompt() {
	      const promptEl = document.getElementById('agentPrompt');
	      let msg = (promptEl.value || '').trim();
      if (!msg) return;

      if (document.getElementById('agentVision').checked && !msg.includes('--vision')) msg += ' --vision';
      if (document.getElementById('agentCodex').checked && !msg.endsWith('--codex')) msg += ' --codex';
      if (document.getElementById('agentGemini').checked && !msg.endsWith('--gemini')) msg += ' --gemini';

      const session = (document.getElementById('agentSession').value || '').trim();
      const out = await api('/api/agent', 'POST', {message: msg, session});
      setText('agentOut', out.output || out.error || '');
      appendOps(`agent: ${out.ok ? 'ok' : 'failed'}`);
    }

    async function runOnboard() {
      const out = await api('/api/onboard', 'POST', {});
      appendOps(out.output || out.error || 'onboard complete');
      refreshAll();
    }

    async function cronAdd() {
      const body = {
        name: document.getElementById('cronName').value,
        message: document.getElementById('cronMsg').value,
        every: document.getElementById('cronEvery').value,
        cron: document.getElementById('cronExpr').value,
        at: document.getElementById('cronAt').value,
      };
      const out = await api('/api/cron/add', 'POST', body);
      appendOps(out.output || out.error || 'cron add');
      refreshAll();
    }

    async function cronRemove() {
      const body = {job_id: document.getElementById('cronRemoveId').value};
      const out = await api('/api/cron/remove', 'POST', body);
      appendOps(out.output || out.error || 'cron remove');
      refreshAll();
    }

    async function gatewayStart() {
      const out = await api('/api/gateway/start', 'POST', {});
      appendOps(out.output || out.error || 'gateway start');
      refreshAll();
    }

	    async function gatewayStop() {
	      const out = await api('/api/gateway/stop', 'POST', {});
	      appendOps(out.output || out.error || 'gateway stop');
	      refreshAll();
	    }

	    setInterval(refreshGatewayLog, 2000);
	    refreshAll();
	    configReload();
	  </script>
</body>
</html>
"""


class DashboardState:
    def __init__(self, attoclaw_bin: str):
        self.attoclaw_bin = attoclaw_bin
        self.gateway_proc = None
        self.gateway_started = None
        self.gateway_log = deque(maxlen=600)
        self.lock = threading.Lock()
        self.config_path = os.path.expanduser("~/.attoclaw/config.json")

    def run_attoclaw(self, args, timeout=120):
        cmd = [self.attoclaw_bin] + args
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
            output = (proc.stdout or "") + (proc.stderr or "")
            return {
                "ok": proc.returncode == 0,
                "code": proc.returncode,
                "output": output.strip(),
                "command": " ".join(cmd),
            }
        except subprocess.TimeoutExpired:
            return {
                "ok": False,
                "code": -1,
                "output": f"Command timed out after {timeout}s",
                "command": " ".join(cmd),
            }
        except Exception as exc:
            return {
                "ok": False,
                "code": -1,
                "output": f"Command failed: {exc}",
                "command": " ".join(cmd),
            }

    def _read_gateway_output(self):
        proc = self.gateway_proc
        if not proc or not proc.stdout:
            return
        for line in proc.stdout:
            with self.lock:
                self.gateway_log.append(line.rstrip())

    def gateway_status(self):
        with self.lock:
            running = self.gateway_proc is not None and self.gateway_proc.poll() is None
            pid = self.gateway_proc.pid if running else None
            return {"running": running, "pid": pid, "started_at": self.gateway_started}

    def gateway_start(self):
        with self.lock:
            if self.gateway_proc is not None and self.gateway_proc.poll() is None:
                return {"ok": True, "output": "Gateway is already running."}

            try:
                proc = subprocess.Popen(
                    [self.attoclaw_bin, "gateway"],
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    bufsize=1,
                )
            except Exception as exc:
                return {"ok": False, "error": f"Failed to start gateway: {exc}"}

            self.gateway_proc = proc
            self.gateway_started = int(time.time())
            self.gateway_log.clear()
            thread = threading.Thread(target=self._read_gateway_output, daemon=True)
            thread.start()
            return {"ok": True, "output": f"Gateway started (pid {proc.pid})."}

    def gateway_stop(self):
        with self.lock:
            proc = self.gateway_proc
            if proc is None or proc.poll() is not None:
                self.gateway_proc = None
                return {"ok": True, "output": "Gateway is not running."}

        try:
            if proc.stdin:
                proc.stdin.write("\n")
                proc.stdin.flush()
            proc.wait(timeout=8)
        except Exception:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except Exception:
                proc.kill()

        with self.lock:
            self.gateway_proc = None
        return {"ok": True, "output": "Gateway stopped."}

    def _mask_config(self, cfg: dict):
        def mask_val(v: str) -> str:
            if not isinstance(v, str) or not v:
                return v
            if len(v) <= 6:
                return "***"
            return v[:3] + "***" + v[-3:]

        secret_keys = {"apikey", "token", "password", "bridgetoken", "authorization", "secret", "key"}

        def walk(node, key_hint=""):
            if isinstance(node, dict):
                out = {}
                for k, v in node.items():
                    kh = (k or "").lower()
                    if any(sk in kh for sk in secret_keys) and isinstance(v, str) and v.strip():
                        out[k] = mask_val(v)
                    else:
                        out[k] = walk(v, kh)
                return out
            if isinstance(node, list):
                return [walk(x, key_hint) for x in node]
            return node

        return walk(cfg)

    def read_config(self, raw: bool = False):
        path = self.config_path
        if not os.path.exists(path):
            return {"ok": True, "exists": False, "path": path, "config": {}}
        try:
            with open(path, "r", encoding="utf-8") as f:
                raw = f.read()
            cfg = json.loads(raw) if raw.strip() else {}
            if not isinstance(cfg, dict):
                return {"ok": False, "error": "config.json must contain a JSON object", "path": path}
            if raw:
                return {"ok": True, "exists": True, "path": path, "config": cfg}
            return {"ok": True, "exists": True, "path": path, "config": self._mask_config(cfg)}
        except Exception as exc:
            return {"ok": False, "error": f"failed to read config: {exc}", "path": path}

    def write_config(self, cfg: dict):
        path = self.config_path
        os.makedirs(os.path.dirname(path), exist_ok=True)
        if os.path.exists(path):
            ts = time.strftime("%Y%m%d_%H%M%S")
            backup = f"{path}.bak.{ts}"
            try:
                shutil.copy2(path, backup)
            except Exception:
                pass
        with open(path, "w", encoding="utf-8") as f:
            json.dump(cfg, f, indent=2, sort_keys=True)
            f.write("\n")
        return {"ok": True, "path": path}

    def restart_attoclaw_after_config_change(self):
        # In dashboard context, the only long-lived AttoClaw process is the gateway we manage.
        st = self.gateway_status()
        if not st.get("running"):
            return {"ok": True, "output": "Config saved. Gateway was not running; no restart needed."}
        stop = self.gateway_stop()
        start = self.gateway_start()
        ok = bool(stop.get("ok")) and bool(start.get("ok"))
        msg = "Config saved. Gateway restarted." if ok else "Config saved. Gateway restart failed."
        return {"ok": ok, "output": msg, "stop": stop, "start": start}


def clean_agent_output(raw: str) -> str:
    text = (raw or "").strip()
    if text.startswith("AttoClaw\n"):
        return text.split("\n", 1)[1].strip()
    if text.startswith("AttoClaw\r\n"):
        return "\n".join(text.splitlines()[1:]).strip()
    return text


def json_response(handler, payload, code=200):
    raw = json.dumps(payload).encode("utf-8")
    handler.send_response(code)
    handler.send_header("Content-Type", "application/json")
    handler.send_header("Content-Length", str(len(raw)))
    handler.end_headers()
    handler.wfile.write(raw)


class Handler(BaseHTTPRequestHandler):
    state: DashboardState = None

    def log_message(self, fmt, *args):
        return

    def _body(self):
        length = int(self.headers.get("Content-Length", "0") or "0")
        if length <= 0:
            return {}
        raw = self.rfile.read(length)
        try:
            return json.loads(raw.decode("utf-8"))
        except Exception:
            return {}

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path
        qs = parse_qs(parsed.query or "")
        if path == "/":
            raw = HTML.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)
            return

        if path == "/api/summary":
            status = self.state.run_attoclaw(["status"], timeout=30)
            channels = self.state.run_attoclaw(["channels", "status"], timeout=30)
            cron = self.state.run_attoclaw(["cron", "list"], timeout=30)
            json_response(self, {
                "bin": self.state.attoclaw_bin,
                "status": status["output"],
                "channels": channels["output"],
                "cron": cron["output"],
                "gateway": self.state.gateway_status(),
            })
            return

        if path == "/api/gateway/log":
            with self.state.lock:
                lines = list(self.state.gateway_log)
            json_response(self, {"ok": True, "lines": lines})
            return

        if path == "/api/config":
            raw = (qs.get("raw") or ["0"])[0] in ("1", "true", "yes")
            data = self.state.read_config(raw=raw)
            json_response(self, data, code=200 if data.get("ok") else 500)
            return

        if path == "/api/doctor":
            out = self.state.run_attoclaw(["doctor"], timeout=30)
            json_response(self, out)
            return

        if path == "/api/metrics":
            mpath = os.path.expanduser("~/.attoclaw/state/metrics.json")
            try:
                if os.path.exists(mpath):
                    with open(mpath, "r", encoding="utf-8") as f:
                        raw = f.read()
                    payload = json.loads(raw) if raw.strip() else {}
                else:
                    payload = {}
                json_response(self, {"ok": True, "metrics": payload})
            except Exception as exc:
                json_response(self, {"ok": False, "error": str(exc)}, code=500)
            return

        json_response(self, {"ok": False, "error": "Not found"}, code=404)

    def do_POST(self):
        path = urlparse(self.path).path
        body = self._body()

        if path == "/api/onboard":
            result = self.state.run_attoclaw(["onboard"], timeout=120)
            json_response(self, result)
            return

        if path == "/api/agent":
            msg = (body.get("message") or "").strip()
            session = (body.get("session") or "").strip()
            if not msg:
                json_response(self, {"ok": False, "error": "message is required"}, code=400)
                return
            args = ["agent", "-m", msg]
            if session:
                args += ["-s", session]
            result = self.state.run_attoclaw(args, timeout=600)
            result["output"] = clean_agent_output(result.get("output", ""))
            json_response(self, result)
            return

        if path == "/api/cron/add":
            name = (body.get("name") or "job").strip() or "job"
            message = (body.get("message") or "").strip()
            every = (body.get("every") or "").strip()
            cron_expr = (body.get("cron") or "").strip()
            at = (body.get("at") or "").strip()

            if not message:
                json_response(self, {"ok": False, "error": "message is required"}, code=400)
                return

            args = ["cron", "add", "--name", name, "--message", message]
            if every:
                args += ["--every", every]
            elif cron_expr:
                args += ["--cron", cron_expr]
            elif at:
                args += ["--at", at]
            else:
                json_response(self, {"ok": False, "error": "set one of every/cron/at"}, code=400)
                return

            result = self.state.run_attoclaw(args, timeout=30)
            json_response(self, result)
            return

        if path == "/api/cron/remove":
            job_id = (body.get("job_id") or "").strip()
            if not job_id:
                json_response(self, {"ok": False, "error": "job_id is required"}, code=400)
                return
            result = self.state.run_attoclaw(["cron", "remove", job_id], timeout=30)
            json_response(self, result)
            return

        if path == "/api/gateway/start":
            json_response(self, self.state.gateway_start())
            return

        if path == "/api/gateway/stop":
            json_response(self, self.state.gateway_stop())
            return

        if path == "/api/config":
            cfg = body.get("config")
            if not isinstance(cfg, dict):
                json_response(self, {"ok": False, "error": "config must be a JSON object"}, code=400)
                return
            with self.state.lock:
                w = self.state.write_config(cfg)
                if not w.get("ok"):
                    json_response(self, {"ok": False, "error": w.get("error", "write failed")}, code=500)
                    return
                r = self.state.restart_attoclaw_after_config_change()
            json_response(self, {"ok": r.get("ok", True), "output": r.get("output", "config saved")})
            return

        if path == "/api/send":
            channel = (body.get("channel") or "").strip()
            to = (body.get("to") or "").strip()
            message = (body.get("message") or "").strip()
            if not channel or not to or not message:
                json_response(self, {"ok": False, "error": "channel/to/message required"}, code=400)
                return
            result = self.state.run_attoclaw(["send", "--channel", channel, "--to", to, "--message", message], timeout=60)
            json_response(self, result)
            return

        json_response(self, {"ok": False, "error": "Not found"}, code=404)


def resolve_attoclaw_bin(explicit_bin: str) -> str:
    candidates = []
    if explicit_bin:
        candidates.append(explicit_bin)

    cwd = os.getcwd()
    candidates.extend([
        os.path.join(cwd, "build", "attoclaw"),
        os.path.join(cwd, "build", "Release", "attoclaw.exe"),
        os.path.join(cwd, "attoclaw"),
        os.path.join(cwd, "attoclaw.exe"),
    ])

    for item in candidates:
        if item and os.path.exists(item) and os.access(item, os.X_OK):
            return os.path.abspath(item)

    from_path = shutil.which("attoclaw")
    if from_path:
        return os.path.abspath(from_path)

    raise RuntimeError("Could not locate attoclaw binary. Use --bin /path/to/attoclaw")


def main():
    parser = argparse.ArgumentParser(description="AttoClaw dashboard server")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8787)
    parser.add_argument("--bin", dest="bin_path", default="")
    args = parser.parse_args()

    attoclaw_bin = resolve_attoclaw_bin(args.bin_path)
    state = DashboardState(attoclaw_bin)
    Handler.state = state

    server = ThreadingHTTPServer((args.host, args.port), Handler)

    def handle_signal(*_):
        raise KeyboardInterrupt

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    print(f"AttoClaw dashboard running at http://{args.host}:{args.port}")
    print(f"Using binary: {attoclaw_bin}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        try:
            state.gateway_stop()
        except Exception:
            pass
        server.server_close()


if __name__ == "__main__":
    main()
