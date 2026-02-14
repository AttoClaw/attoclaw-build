# AttoClaw

<p align="center">
<img src="./attoclaw.png" alt="AttoClaw Logo" width="1250"/>
</p>

AttoClaw is a high-performance C++ port of openclaw focused on low overhead, deterministic runtime behavior, and practical local automation.

This repo currently builds a single binary:

- `attoclaw.exe` (Windows)
- `attoclaw` (Linux/macOS)

## Current scope

Implemented and stable:

- Core CLI: `onboard`, `status`, `agent`, `gateway`, `channels`, `cron`
- Agent loop with tool calling
- Session persistence and memory files
- Telegram and WhatsApp channels
- NVIDIA NIM (OpenAI-compatible API)
- Vision mode and OCR support
- Runtime `/stop` cancellation
- Performance benchmark script


## Requirements

Build:

- CMake `>= 3.20`
- C++20 compiler
- `libcurl` (required)
- Windows: Visual Studio C++ toolchain recommended

Runtime:

- API key for at least one provider (`openai`, `openrouter`, or `nim`)
- Node.js + npm for WhatsApp bridge login flow
- Tesseract OCR (optional, used by vision OCR mode)

Notes:

- The generated WhatsApp bridge package declares `node >= 20`.
- `attoclaw onboard` tries to auto-install Tesseract on Windows via `winget`, then `choco`, then `scoop`.

## Build

Windows (your current setup):

```powershell
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="C:/vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
```

Result binary:

- `build/Release/attoclaw.exe`

Generic:

```bash
cmake -S . -B build
cmake --build build --config Release
```

## Install and first run

From repo root:

```powershell
build/Release/attoclaw.exe onboard
build/Release/attoclaw.exe status
```

On first run, AttoClaw creates:

- `~/.attoclaw/config.json`
- `~/.attoclaw/workspace`
- workspace templates (`AGENTS.md`, `SOUL.md`, `USER.md`, memory files, etc.)

## Configuration

Config file:

- `~/.attoclaw/config.json`

Default high-level structure:

```json
{
  "providers": {
    "openrouter": { "apiKey": "", "apiBase": "https://openrouter.ai/api/v1" },
    "openai": { "apiKey": "", "apiBase": "https://api.openai.com/v1" },
    "nim": { "apiKey": "", "apiBase": "https://integrate.api.nvidia.com/v1" }
  },
  "agents": {
    "defaults": {
      "workspace": "~/.attoclaw/workspace",
      "model": "openai/gpt-4o-mini",
      "maxTokens": 2048,
      "temperature": 0.7,
      "topP": 0.9,
      "maxToolIterations": 10,
      "memoryWindow": 24
    }
  },
  "tools": {
    "exec": { "timeout": 60 },
    "web": { "search": { "apiKey": "", "maxResults": 5 } },
    "restrictToWorkspace": false
  },
  "channels": {
    "whatsapp": { "enabled": false, "bridgeUrl": "ws://localhost:3001", "bridgeToken": "", "allowFrom": [] },
    "telegram": { "enabled": false, "token": "", "allowFrom": [], "proxy": "" }
  }
}
```

Provider selection behavior:

- Provider is inferred from `agents.defaults.model` prefix/keywords.
- Env var references like `"$NVIDIA_API_KEY"` and `"${NVIDIA_API_KEY}"` are resolved.
- Supported env fallbacks:
  - `OPENAI_API_KEY`
  - `OPENROUTER_API_KEY`
  - `NVIDIA_API_KEY`

## Command reference

```text
attoclaw onboard
attoclaw status
attoclaw agent [-m MESSAGE] [-s SESSION] [--vision] [--vision-fps FPS] [--vision-frames N]
attoclaw gateway
attoclaw channels status
attoclaw channels login
attoclaw cron list
attoclaw cron add --name NAME --message MSG [--every SEC | --cron EXPR | --at ISO]
attoclaw cron remove JOB_ID
attoclaw --version
```

## Running AttoClaw

One-shot chat:

```powershell
build/Release/attoclaw.exe agent -m "Hello"
```

Interactive chat:

```powershell
build/Release/attoclaw.exe agent
```

Interactive control commands:

- `/new` start a fresh session
- `/help` show quick command help
- `/stop` request immediate cancellation of current in-flight task

## Vision and screen understanding

AttoClaw has two vision pathways:

1. Dedicated live vision mode (CLI flag):

```powershell
build/Release/attoclaw.exe agent -m "Track on-screen changes" --vision --vision-fps 2 --vision-frames 60
```

- Captures frames continuously
- Sends frame image + OCR text to the model
- Falls back to OCR/text-only if image ingestion fails
- Current implementation target: Windows

2. Tool-level screen capture during normal agent turn:

- `screen_capture` tool is disabled by default.
- It is enabled only if the user message contains `--vision`.
- This applies to both main agent turns and subagents.

Example:

```text
Analyze this issue and capture screen context --vision
```

## Channels

### Telegram

Config:

```json
{
  "channels": {
    "telegram": {
      "enabled": true,
      "token": "YOUR_BOT_TOKEN",
      "allowFrom": ["YOUR_USER_ID"]
    }
  }
}
```

`allowFrom` behavior:

- Empty array means allow all
- If non-empty, only listed sender IDs/usernames are processed

### WhatsApp (bridge-based)

1. Bootstrap/login bridge:

```powershell
build/Release/attoclaw.exe channels login
```

This creates/uses:

- `~/.attoclaw/bridge`
- `~/.attoclaw/whatsapp-auth`

2. Enable in config:

```json
{
  "channels": {
    "whatsapp": {
      "enabled": true,
      "bridgeUrl": "ws://localhost:3001",
      "bridgeToken": "",
      "allowFrom": []
    }
  }
}
```

3. Run gateway:

```powershell
build/Release/attoclaw.exe gateway
```

`allowFrom` behavior:

- Empty array means allow all senders
- If non-empty, sender IDs must match

## Scheduled tasks (cron)

Examples:

```powershell
build/Release/attoclaw.exe cron add --name hourly --message "status check" --every 3600
build/Release/attoclaw.exe cron add --name morning --message "daily summary" --cron "0 9 * * *"
build/Release/attoclaw.exe cron add --name once --message "one-time task" --at "2026-02-15T10:30:00"
build/Release/attoclaw.exe cron list
build/Release/attoclaw.exe cron run <job_id>
build/Release/attoclaw.exe cron remove <job_id>
```

Gateway mode also executes cron via internal callback and can deliver responses to channels.

## Tools implemented

Core toolset currently available:

- `read_file`
- `write_file`
- `edit_file`
- `list_dir`
- `exec`
- `web_search`
- `web_fetch`
- `system_inspect`
- `app_control`
- `screen_capture` (vision-gated)
- `message`
- `spawn`
- `cron`

## Performance optimizations implemented

- Lock-free bounded MPMC queue on message bus
- Semaphore-based wakeups
- Reduced queue capacity baseline for lower memory
- Adaptive queue backoff (yield then short sleep)
- Cached tool schema JSON (no repeated rebuild each turn)
- Lighter default agent limits (`maxTokens`, `maxToolIterations`, `memoryWindow`)
- Release optimization improvements:
  - MSVC: `/GL`, `/LTCG`, `/OPT:REF`, `/OPT:ICF`
  - GCC/Clang: section splitting + GC sections
  - IPO/LTO enabled when supported

## Benchmarking

Benchmark script:

- `scripts/benchmark_compare.py`

What it measures:

- CLI startup latency
- Single-turn agent latency
- Approximate peak RSS memory

Compared targets (when installed):

- AttoClaw
- nanobot
- OpenClaw

### Performance comparison (quick run)

| Metric | AttoClaw | nanobot | OpenClaw |
|---|---:|---:|---:|
| CLI startup (`--version`) avg ms | 35.9 | 664.6 | 3678.8 |
| Agent one-shot avg ms | 64.5 | 4929.6 | N/A (local run failed in test profile) |
| Agent peak RSS MB | 7.3 | 162.2 | N/A (local run failed in test profile) |

*This is an initial, non-final benchmark and does not represent a stable comparison.*

Quick mode:

```powershell
python scripts/benchmark_compare.py --quick
```

Full mode:

```powershell
python scripts/benchmark_compare.py
```

Requirements for cross-project benchmark:

```powershell
python -m pip install nanobot-ai psutil
npm install -g openclaw
```

Important benchmark note:

- The script temporarily writes test configs for AttoClaw and nanobot and restores backups on exit.
- OpenClaw local-agent runs may fail if your OpenClaw profile is pinned to provider-specific auth/model settings incompatible with the mock server.

## Troubleshooting

`Error calling LLM (HTTP 404)`:

- Usually wrong `apiBase` for the selected model/provider
- Verify `providers.*.apiBase` and model/provider alignment

WhatsApp warnings about libcurl protocol/options:

- Build/link against a `libcurl` with WebSocket support
- In vcpkg, ensure websocket-enabled curl variant is used

`channels login` fails with npm/node issues:

- Install Node.js and npm
- Use Node 20+ for bridge compatibility

No replies from channel:

- Check `allowFrom` filtering
- Check gateway is running
- Check provider key/model configuration

## Project layout

```text
attoclaw-build/
  include/attoclaw/    # runtime headers (agent, tools, channels, config, etc.)
  src/main.cpp        # CLI entrypoint
  scripts/            # benchmark and helper scripts
  CMakeLists.txt      # build config
```

