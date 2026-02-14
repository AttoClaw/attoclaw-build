#include <algorithm>
#include <ctime>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "attoclaw/agent.hpp"
#include "attoclaw/channels.hpp"
#include "attoclaw/config.hpp"
#include "attoclaw/cron.hpp"
#include "attoclaw/heartbeat.hpp"
#include "attoclaw/provider.hpp"
#include "attoclaw/telegram_channel.hpp"
#include "attoclaw/vision.hpp"
#include "attoclaw/whatsapp_channel.hpp"

namespace {

using namespace attoclaw;

void print_usage() {
  std::cout
      << "AttoClaw - C++ ultra-fast personal AI assistant\n\n"
      << "Usage:\n"
      << "  attoclaw onboard\n"
      << "  attoclaw status\n"
      << "  attoclaw agent [-m MESSAGE] [-s SESSION] [--vision] [--vision-fps FPS] [--vision-frames N]\n"
      << "  attoclaw gateway\n"
      << "  attoclaw channels status\n"
      << "  attoclaw channels login\n"
      << "  attoclaw cron list\n"
      << "  attoclaw cron add --name NAME --message MSG [--every SECONDS | --cron EXPR | --at ISO]\n"
      << "  attoclaw cron remove JOB_ID\n"
      << "  attoclaw --version\n";
}

bool has_flag(const std::vector<std::string>& args, const std::string& flag) {
  return std::find(args.begin(), args.end(), flag) != args.end();
}

std::string get_flag_value(const std::vector<std::string>& args, const std::string& flag,
                           const std::string& fallback = "") {
  for (std::size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] == flag) {
      return args[i + 1];
    }
  }
  return fallback;
}

int get_int_flag_value(const std::vector<std::string>& args, const std::string& flag, int fallback, int min_value,
                       int max_value) {
  const std::string raw = trim(get_flag_value(args, flag, std::to_string(fallback)));
  try {
    const int v = std::stoi(raw);
    return std::clamp(v, min_value, max_value);
  } catch (...) {
    return fallback;
  }
}

bool command_exists(const std::string& command) {
#ifdef _WIN32
  const std::string probe = "where " + command;
#else
  const std::string probe = "command -v " + command;
#endif
  const CommandResult out = run_command_capture(probe, 10);
  return out.ok && !trim(out.output).empty();
}

bool install_tesseract_onboard() {
#ifndef _WIN32
  return true;
#else
  if (command_exists("tesseract")) {
    std::cout << "Tesseract OCR: already installed\n";
    return true;
  }

  std::cout << "Tesseract OCR: not found. Attempting automatic install...\n";

  auto try_cmd = [](const std::string& cmd, int timeout_s = 240) -> bool {
    const CommandResult r = run_command_capture(cmd, timeout_s);
    return r.ok;
  };

  if (command_exists("winget")) {
    const std::vector<std::string> winget_ids = {
        "UB-Mannheim.TesseractOCR",
        "tesseract-ocr.tesseract",
    };
    for (const auto& id : winget_ids) {
      const std::string cmd =
          "winget install -e --id " + id +
          " --accept-package-agreements --accept-source-agreements --disable-interactivity --silent";
      if (try_cmd(cmd) && command_exists("tesseract")) {
        std::cout << "Tesseract OCR: installed via winget (" << id << ")\n";
        return true;
      }
    }
  }

  if (command_exists("choco")) {
    if (try_cmd("choco install tesseract -y --no-progress", 240) && command_exists("tesseract")) {
      std::cout << "Tesseract OCR: installed via choco\n";
      return true;
    }
  }

  if (command_exists("scoop")) {
    if (try_cmd("scoop install tesseract", 240) && command_exists("tesseract")) {
      std::cout << "Tesseract OCR: installed via scoop\n";
      return true;
    }
  }

  std::cout << "Tesseract OCR: automatic install failed.\n";
  std::cout << "Install manually with one of:\n";
  std::cout << "  winget install -e --id UB-Mannheim.TesseractOCR\n";
  std::cout << "  choco install tesseract -y\n";
  std::cout << "Then restart terminal so `tesseract` is in PATH.\n";
  return false;
#endif
}

bool ensure_home_bridge(const fs::path& bridge_dir) {
  const fs::path src_dir = bridge_dir / "src";
  std::error_code ec;
  fs::create_directories(src_dir, ec);

  const std::vector<std::pair<fs::path, std::string>> files = {
      {bridge_dir / "package.json",
       R"({
  "name": "attoclaw-whatsapp-bridge",
  "version": "0.1.0",
  "description": "WhatsApp bridge for AttoClaw using Baileys",
  "type": "module",
  "main": "dist/index.js",
  "scripts": {
    "build": "tsc",
    "start": "node dist/index.js",
    "dev": "tsc && node dist/index.js"
  },
  "dependencies": {
    "@whiskeysockets/baileys": "7.0.0-rc.9",
    "qrcode-terminal": "^0.12.0",
    "pino": "^9.0.0",
    "ws": "^8.17.1"
  },
  "devDependencies": {
    "@types/node": "^20.14.0",
    "@types/ws": "^8.5.10",
    "typescript": "^5.4.0"
  },
  "engines": {
    "node": ">=20.0.0"
  }
})"},
      {bridge_dir / "tsconfig.json",
       R"({
  "compilerOptions": {
    "target": "ES2022",
    "module": "NodeNext",
    "moduleResolution": "NodeNext",
    "outDir": "dist",
    "rootDir": "src",
    "strict": true,
    "esModuleInterop": true,
    "skipLibCheck": true,
    "forceConsistentCasingInFileNames": true
  },
  "include": ["src/**/*.ts", "src/**/*.d.ts"]
})"},
      {src_dir / "index.ts",
       R"(#!/usr/bin/env node
import { webcrypto } from 'crypto';
if (!globalThis.crypto) {
  (globalThis as any).crypto = webcrypto;
}

import { BridgeServer } from './server.js';
import { homedir } from 'os';
import { join } from 'path';

const PORT = parseInt(process.env.BRIDGE_PORT || '3001', 10);
const AUTH_DIR = process.env.AUTH_DIR || join(homedir(), '.attoclaw', 'whatsapp-auth');
const TOKEN = process.env.BRIDGE_TOKEN || undefined;

console.log('AttoClaw WhatsApp Bridge');
console.log('=======================\n');

const server = new BridgeServer(PORT, AUTH_DIR, TOKEN);

process.on('SIGINT', async () => {
  console.log('\n\nShutting down...');
  await server.stop();
  process.exit(0);
});

process.on('SIGTERM', async () => {
  await server.stop();
  process.exit(0);
});

server.start().catch((error) => {
  console.error('Failed to start bridge:', error);
  process.exit(1);
});
)"},
      {src_dir / "server.ts",
       R"(import { WebSocketServer, WebSocket } from 'ws';
import { WhatsAppClient } from './whatsapp.js';

interface SendCommand {
  type: 'send';
  to: string;
  text: string;
}

interface BridgeMessage {
  type: 'message' | 'status' | 'qr' | 'error';
  [key: string]: unknown;
}

export class BridgeServer {
  private wss: WebSocketServer | null = null;
  private wa: WhatsAppClient | null = null;
  private clients: Set<WebSocket> = new Set();

  constructor(private port: number, private authDir: string, private token?: string) {}

  async start(): Promise<void> {
    this.wss = new WebSocketServer({ host: '127.0.0.1', port: this.port });
    console.log(`Bridge server listening on ws://127.0.0.1:${this.port}`);
    if (this.token) console.log('Token authentication enabled');

    this.wa = new WhatsAppClient({
      authDir: this.authDir,
      onMessage: (msg) => this.broadcast({ type: 'message', ...msg }),
      onQR: (qr) => this.broadcast({ type: 'qr', qr }),
      onStatus: (status) => this.broadcast({ type: 'status', status }),
    });

    this.wss.on('connection', (ws) => {
      if (this.token) {
        const timeout = setTimeout(() => ws.close(4001, 'Auth timeout'), 5000);
        ws.once('message', (data) => {
          clearTimeout(timeout);
          try {
            const msg = JSON.parse(data.toString());
            if (msg.type === 'auth' && msg.token === this.token) {
              console.log('AttoClaw client authenticated');
              this.setupClient(ws);
            } else {
              ws.close(4003, 'Invalid token');
            }
          } catch {
            ws.close(4003, 'Invalid auth message');
          }
        });
      } else {
        console.log('AttoClaw client connected');
        this.setupClient(ws);
      }
    });

    await this.wa.connect();
  }

  private setupClient(ws: WebSocket): void {
    this.clients.add(ws);

    ws.on('message', async (data) => {
      try {
        const cmd = JSON.parse(data.toString()) as SendCommand;
        await this.handleCommand(cmd);
        ws.send(JSON.stringify({ type: 'sent', to: cmd.to }));
      } catch (error) {
        console.error('Error handling command:', error);
        ws.send(JSON.stringify({ type: 'error', error: String(error) }));
      }
    });

    ws.on('close', () => {
      console.log('AttoClaw client disconnected');
      this.clients.delete(ws);
    });

    ws.on('error', (error) => {
      console.error('WebSocket error:', error);
      this.clients.delete(ws);
    });
  }

  private async handleCommand(cmd: SendCommand): Promise<void> {
    if (cmd.type === 'send' && this.wa) {
      await this.wa.sendMessage(cmd.to, cmd.text);
    }
  }

  private broadcast(msg: BridgeMessage): void {
    const data = JSON.stringify(msg);
    for (const client of this.clients) {
      if (client.readyState === WebSocket.OPEN) {
        client.send(data);
      }
    }
  }

  async stop(): Promise<void> {
    for (const client of this.clients) {
      client.close();
    }
    this.clients.clear();

    if (this.wss) {
      this.wss.close();
      this.wss = null;
    }

    if (this.wa) {
      await this.wa.disconnect();
      this.wa = null;
    }
  }
}
)"},
      {src_dir / "whatsapp.ts",
       R"(/* eslint-disable @typescript-eslint/no-explicit-any */
import makeWASocket, {
  DisconnectReason,
  useMultiFileAuthState,
  fetchLatestBaileysVersion,
  makeCacheableSignalKeyStore,
} from '@whiskeysockets/baileys';

import { Boom } from '@hapi/boom';
import qrcode from 'qrcode-terminal';
import pino from 'pino';

const VERSION = '0.1.0';

export interface InboundMessage {
  id: string;
  sender: string;
  pn: string;
  content: string;
  timestamp: number;
  isGroup: boolean;
}

export interface WhatsAppClientOptions {
  authDir: string;
  onMessage: (msg: InboundMessage) => void;
  onQR: (qr: string) => void;
  onStatus: (status: string) => void;
}

export class WhatsAppClient {
  private sock: any = null;
  private options: WhatsAppClientOptions;
  private reconnecting = false;

  constructor(options: WhatsAppClientOptions) {
    this.options = options;
  }

  async connect(): Promise<void> {
    const logger = pino({ level: 'silent' });
    const { state, saveCreds } = await useMultiFileAuthState(this.options.authDir);
    const { version } = await fetchLatestBaileysVersion();

    console.log(`Using Baileys version: ${version.join('.')}`);

    this.sock = makeWASocket({
      auth: {
        creds: state.creds,
        keys: makeCacheableSignalKeyStore(state.keys, logger),
      },
      version,
      logger,
      printQRInTerminal: false,
      browser: ['attoclaw', 'cli', VERSION],
      syncFullHistory: false,
      markOnlineOnConnect: false,
    });

    if (this.sock.ws && typeof this.sock.ws.on === 'function') {
      this.sock.ws.on('error', (err: Error) => {
        console.error('WebSocket error:', err.message);
      });
    }

    this.sock.ev.on('connection.update', async (update: any) => {
      const { connection, lastDisconnect, qr } = update;

      if (qr) {
        console.log('\nScan this QR code with WhatsApp (Linked Devices):\n');
        qrcode.generate(qr, { small: true });
        this.options.onQR(qr);
      }

      if (connection === 'close') {
        const statusCode = (lastDisconnect?.error as Boom)?.output?.statusCode;
        const shouldReconnect = statusCode !== DisconnectReason.loggedOut;

        console.log(`Connection closed. Status: ${statusCode}, Will reconnect: ${shouldReconnect}`);
        this.options.onStatus('disconnected');

        if (shouldReconnect && !this.reconnecting) {
          this.reconnecting = true;
          console.log('Reconnecting in 5 seconds...');
          setTimeout(() => {
            this.reconnecting = false;
            this.connect();
          }, 5000);
        }
      } else if (connection === 'open') {
        console.log('Connected to WhatsApp');
        this.options.onStatus('connected');
      }
    });

    this.sock.ev.on('creds.update', saveCreds);

    this.sock.ev.on('messages.upsert', async ({ messages, type }: { messages: any[]; type: string }) => {
      if (type !== 'notify') return;

      for (const msg of messages) {
        if (msg.key.fromMe) continue;
        if (msg.key.remoteJid === 'status@broadcast') continue;

        const content = this.extractMessageContent(msg);
        if (!content) continue;

        const isGroup = msg.key.remoteJid?.endsWith('@g.us') || false;

        this.options.onMessage({
          id: msg.key.id || '',
          sender: msg.key.remoteJid || '',
          pn: msg.key.remoteJidAlt || '',
          content,
          timestamp: msg.messageTimestamp as number,
          isGroup,
        });
      }
    });
  }

  private extractMessageContent(msg: any): string | null {
    const message = msg.message;
    if (!message) return null;

    if (message.conversation) {
      return message.conversation;
    }
    if (message.extendedTextMessage?.text) {
      return message.extendedTextMessage.text;
    }
    if (message.imageMessage?.caption) {
      return `[Image] ${message.imageMessage.caption}`;
    }
    if (message.videoMessage?.caption) {
      return `[Video] ${message.videoMessage.caption}`;
    }
    if (message.documentMessage?.caption) {
      return `[Document] ${message.documentMessage.caption}`;
    }
    if (message.audioMessage) {
      return '[Voice Message]';
    }

    return null;
  }

  async sendMessage(to: string, text: string): Promise<void> {
    if (!this.sock) {
      throw new Error('Not connected');
    }
    await this.sock.sendMessage(to, { text });
  }

  async disconnect(): Promise<void> {
    if (this.sock) {
      this.sock.end(undefined);
      this.sock = null;
    }
  }
}
)"},
      {src_dir / "types.d.ts",
       R"(declare module 'qrcode-terminal' {
  interface QRCodeTerminal {
    generate(text: string, opts?: { small?: boolean }): void;
  }
  const qrcode: QRCodeTerminal;
  export default qrcode;
}
)"},
  };

  for (const auto& [path, content] : files) {
    if (!fs::exists(path)) {
      if (!write_text_file(path, content)) {
        return false;
      }
    }
  }
  return true;
}

std::string shell_in_dir_command(const fs::path& dir, const std::string& command) {
#ifdef _WIN32
  return "cd /d \"" + dir.string() + "\" && " + command;
#else
  return "cd \"" + dir.string() + "\" && " + command;
#endif
}

void create_workspace_templates(const fs::path& workspace) {
  std::error_code ec;
  fs::create_directories(workspace / "memory", ec);
  fs::create_directories(workspace / "skills", ec);

  const std::vector<std::pair<fs::path, std::string>> files = {
      {workspace / "AGENTS.md",
       "# Agent Instructions\n\n"
       "You are a helpful AI assistant. Be concise, accurate, and friendly.\n"},
      {workspace / "SOUL.md",
       "# Soul\n\n"
       "I am AttoClaw, a high-performance AI assistant.\n"},
      {workspace / "USER.md",
       "# User\n\n"
       "Information about the user and preferences.\n"},
      {workspace / "memory" / "MEMORY.md",
       "# Long-term Memory\n\n"
       "Important facts that should persist across sessions.\n"},
      {workspace / "memory" / "HISTORY.md", ""},
      {workspace / "HEARTBEAT.md",
       "# Heartbeat Tasks\n\n"
       "- [ ] Add background tasks here.\n"},
  };

  for (const auto& [path, content] : files) {
    if (!fs::exists(path)) {
      write_text_file(path, content);
    }
  }
}

OpenAICompatibleProvider make_provider(const Config& cfg) {
  return OpenAICompatibleProvider(cfg.provider.api_key, cfg.provider.api_base, cfg.agent.model);
}

int run_onboard() {
  const fs::path config_path = get_config_path();
  if (fs::exists(config_path)) {
    std::cout << "Config already exists: " << config_path.string() << "\n";
  } else {
    if (!save_default_config(config_path)) {
      std::cerr << "Failed to write config: " << config_path.string() << "\n";
      return 1;
    }
    std::cout << "Created config: " << config_path.string() << "\n";
  }

  Config cfg = load_config(config_path);
  const fs::path workspace = fs::weakly_canonical(expand_user_path(cfg.agent.workspace));
  create_workspace_templates(workspace);
  std::cout << "Workspace ready: " << workspace.string() << "\n";
  install_tesseract_onboard();
  std::cout << "Next: set your API key in " << config_path.string() << "\n";
  return 0;
}

int run_status() {
  const fs::path config_path = get_config_path();
  Config cfg = load_config(config_path);
  const fs::path workspace = fs::weakly_canonical(expand_user_path(cfg.agent.workspace));

  std::cout << "AttoClaw status\n\n";
  std::cout << "Config: " << config_path.string() << (fs::exists(config_path) ? " [ok]" : " [missing]")
            << "\n";
  std::cout << "Workspace: " << workspace.string() << (fs::exists(workspace) ? " [ok]" : " [missing]")
            << "\n";
  std::cout << "Model: " << cfg.agent.model << "\n";
  std::cout << "Provider key: " << (cfg.provider.api_key.empty() ? "not set" : "set") << "\n";
  std::cout << "Provider base: " << cfg.provider.api_base << "\n";
  return 0;
}

int run_agent(const std::vector<std::string>& args) {
  Config cfg = load_config();
  const fs::path workspace = fs::weakly_canonical(expand_user_path(cfg.agent.workspace));
  create_workspace_templates(workspace);

  MessageBus bus;
  auto provider = make_provider(cfg);

  AgentLoop agent(&bus, &provider, workspace, cfg.agent.model, cfg.agent.max_tool_iterations,
                  cfg.agent.temperature, cfg.agent.top_p, cfg.agent.max_tokens, cfg.agent.memory_window,
                  cfg.tools.web_search.api_key, cfg.tools.exec.timeout, cfg.tools.restrict_to_workspace, nullptr);

  const std::string message = get_flag_value(args, "-m", get_flag_value(args, "--message"));
  const std::string session = get_flag_value(args, "-s", get_flag_value(args, "--session", "cli:direct"));
  const bool vision_mode = has_flag(args, "--vision");
  const int vision_fps = get_int_flag_value(args, "--vision-fps", 1, 1, 10);
  const int vision_frames = get_int_flag_value(args, "--vision-frames", 30, 0, 100000);

  if (vision_mode) {
    const std::string prompt = message.empty() ? "Analyze what is visible on this screen frame." : message;
    const int frame_delay_ms = (std::max)(100, 1000 / vision_fps);

#ifndef _WIN32
    std::cerr << "--vision is currently implemented for Windows builds only.\n";
    return 1;
#else
    std::cout << "Vision mode started (" << vision_fps << " FPS, "
              << (vision_frames == 0 ? std::string("unlimited") : std::to_string(vision_frames))
              << " frames). Press Ctrl+C to stop.\n";
    const bool ocr_available = has_tesseract_ocr();
    if (ocr_available) {
      std::cout << "OCR mode: enabled (tesseract detected)\n";
    } else {
      std::cout << "OCR mode: disabled (tesseract not found in PATH)\n";
    }
    std::string prev_summary;

    for (int i = 1; (vision_frames == 0) || (i <= vision_frames); ++i) {
      auto frame = capture_vision_frame(960, 60);
      if (!frame.has_value()) {
        std::cout << "[Vision " << i << "] failed to capture frame\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(frame_delay_ms));
        continue;
      }

      const std::string ocr_text = ocr_available ? extract_ocr_text(frame->path, 20) : "";

      json messages = json::array();
      messages.push_back({{"role", "system"},
                          {"content",
                           "You are AttoClaw in live vision mode. Analyze each incoming screen frame. "
                           "Describe visible UI, changes from prior frame, and relevant actions briefly."}});

      std::ostringstream user_text;
      user_text << prompt << "\n";
      user_text << "Frame " << i << "/"
                << (vision_frames == 0 ? std::string("unlimited") : std::to_string(vision_frames)) << ". ";
      if (!prev_summary.empty()) {
        user_text << "Previous frame summary:\n" << prev_summary << "\n";
      }
      if (!ocr_text.empty()) {
        user_text << "OCR text extracted from current frame:\n" << ocr_text << "\n";
      } else {
        user_text << "OCR text unavailable for this frame.\n";
      }
      user_text << "Use this frame to reason about what is happening right now.";

      json content = json::array();
      content.push_back({{"type", "text"}, {"text", user_text.str()}});
      content.push_back({{"type", "image_url"}, {"image_url", {{"url", frame->data_url}}}});

      messages.push_back({{"role", "user"}, {"content", content}});

      LLMResponse resp =
          provider.chat(messages, json::array(), cfg.agent.model, cfg.agent.max_tokens, cfg.agent.temperature,
                        cfg.agent.top_p);

      if (resp.finish_reason == "error") {
        json fallback = json::array();
        fallback.push_back(messages[0]);
        std::string text_only = user_text.str();
        text_only += "\nImage input failed; continue with OCR/system context only.";
        fallback.push_back({{"role", "user"}, {"content", text_only}});
        resp = provider.chat(fallback, json::array(), cfg.agent.model, cfg.agent.max_tokens, cfg.agent.temperature,
                             cfg.agent.top_p);
      }

      const std::string shown = resp.content.empty() ? "(no response)" : resp.content;
      prev_summary = shown.substr(0, (std::min<std::size_t>)(shown.size(), 1200));
      std::cout << "\n[Vision " << i << "]\n" << shown << "\n";
      std::this_thread::sleep_for(std::chrono::milliseconds(frame_delay_ms));
    }
    return 0;
#endif
  }

  if (!message.empty()) {
    const std::string response = agent.process_direct(message, session, "cli", "direct");
    std::cout << "\nAttoClaw\n" << response << "\n";
    return 0;
  }

  std::cout << "AttoClaw interactive mode (type exit to quit)\n\n";
  while (true) {
    std::cout << "You: ";
    std::string line;
    if (!std::getline(std::cin, line)) {
      break;
    }
    const std::string cmd = trim(line);
    if (cmd.empty()) {
      continue;
    }
    if (cmd == "exit" || cmd == "quit" || cmd == "/exit" || cmd == "/quit") {
      break;
    }

    const std::string response = agent.process_direct(line, session, "cli", "direct");
    std::cout << "\nAttoClaw\n" << response << "\n\n";
  }

  return 0;
}

int run_gateway() {
  Config cfg = load_config();
  const fs::path workspace = fs::weakly_canonical(expand_user_path(cfg.agent.workspace));
  create_workspace_templates(workspace);

  MessageBus bus;
  ChannelManager channel_manager(&bus);
  auto provider = make_provider(cfg);

  const fs::path cron_store = get_data_dir() / "cron" / "jobs.json";
  CronService cron(cron_store);

  AgentLoop agent(&bus, &provider, workspace, cfg.agent.model, cfg.agent.max_tool_iterations,
                  cfg.agent.temperature, cfg.agent.top_p, cfg.agent.max_tokens, cfg.agent.memory_window,
                  cfg.tools.web_search.api_key, cfg.tools.exec.timeout, cfg.tools.restrict_to_workspace, &cron);

  cron.set_on_job([&](const CronJob& job) -> std::optional<std::string> {
    const std::string response =
        agent.process_direct(job.payload.message, "cron:" + job.id, job.payload.channel.empty() ? "cli" : job.payload.channel,
                             job.payload.to.empty() ? "direct" : job.payload.to);

    if (job.payload.deliver && !job.payload.channel.empty() && !job.payload.to.empty()) {
      bus.publish_outbound(OutboundMessage{job.payload.channel, job.payload.to, response});
    }
    return response;
  });

  HeartbeatService heartbeat(workspace, [&](const std::string& prompt) {
    return agent.process_direct(prompt, "heartbeat", "cli", "heartbeat");
  });

  if (cfg.channels.telegram.enabled) {
    channel_manager.add_channel(std::make_shared<TelegramChannel>(cfg.channels.telegram, &bus));
  }
  if (cfg.channels.whatsapp.enabled) {
    channel_manager.add_channel(std::make_shared<WhatsAppChannel>(cfg.channels.whatsapp, &bus));
  }

  const auto enabled_channels = channel_manager.enabled_channels();
  if (!enabled_channels.empty()) {
    std::ostringstream ss;
    for (std::size_t i = 0; i < enabled_channels.size(); ++i) {
      ss << enabled_channels[i];
      if (i + 1 < enabled_channels.size()) {
        ss << ", ";
      }
    }
    std::cout << "Enabled channels: " << ss.str() << "\n";
  } else {
    std::cout << "No channels enabled.\n";
  }

  bus.start_dispatcher();
  channel_manager.start_all();
  cron.start();
  heartbeat.start();
  agent.run();

  std::cout << "AttoClaw gateway started. Press Enter to stop.\n";
  std::string ignored;
  std::getline(std::cin, ignored);

  agent.stop();
  heartbeat.stop();
  cron.stop();
  channel_manager.stop_all();
  bus.stop_dispatcher();
  return 0;
}

int run_channels(const std::vector<std::string>& args) {
  if (args.size() < 2) {
    std::cerr << "Usage: attoclaw channels <status|login>\n";
    return 1;
  }

  Config cfg = load_config();
  const std::string sub = args[1];

  if (sub == "status") {
    std::cout << "Channel Status\n\n";
    std::cout << "WhatsApp: " << (cfg.channels.whatsapp.enabled ? "enabled" : "disabled")
              << " (bridge: " << cfg.channels.whatsapp.bridge_url
              << ", token: " << (cfg.channels.whatsapp.bridge_token.empty() ? "not set" : "set") << ")\n";
    std::cout << "Telegram: " << (cfg.channels.telegram.enabled ? "enabled" : "disabled")
              << " (token: " << (cfg.channels.telegram.token.empty() ? "not set" : "set") << ")\n";
    std::cout << "\nImplemented adapters: Telegram, WhatsApp bridge.\n";
    std::cout << "Other channel adapters removed from AttoClaw.\n";
    return 0;
  }

  if (sub == "login") {
    if (!command_exists("npm")) {
      std::cerr << "npm not found. Install Node.js >= 18 first.\n";
      return 1;
    }

    const fs::path bridge_dir = get_data_dir() / "bridge";
    if (!ensure_home_bridge(bridge_dir)) {
      std::cerr << "Failed to create bridge files under: " << bridge_dir.string() << "\n";
      return 1;
    }

    const bool has_node_modules = fs::exists(bridge_dir / "node_modules");
    if (!has_node_modules) {
      std::cout << "Building WhatsApp bridge in: " << bridge_dir.string() << "\n";
      const CommandResult install =
          run_command_capture(shell_in_dir_command(bridge_dir, "npm install"), 300);
      if (!install.ok) {
        std::cerr << "npm install failed.\n" << install.output << "\n";
        return 1;
      }
    }

    {
      const CommandResult build =
          run_command_capture(shell_in_dir_command(bridge_dir, "npm run build"), 300);
      if (!build.ok) {
        std::cerr << "npm run build failed.\n" << build.output << "\n";
        return 1;
      }
    }

    std::cout << "Starting WhatsApp bridge. Scan QR in this terminal.\n";
#ifdef _WIN32
    if (!cfg.channels.whatsapp.bridge_token.empty()) {
      _putenv_s("BRIDGE_TOKEN", cfg.channels.whatsapp.bridge_token.c_str());
    }
#else
    if (!cfg.channels.whatsapp.bridge_token.empty()) {
      setenv("BRIDGE_TOKEN", cfg.channels.whatsapp.bridge_token.c_str(), 1);
    }
#endif
    const int rc = std::system(shell_in_dir_command(bridge_dir, "npm start").c_str());
#ifdef _WIN32
    if (!cfg.channels.whatsapp.bridge_token.empty()) {
      _putenv_s("BRIDGE_TOKEN", "");
    }
#else
    if (!cfg.channels.whatsapp.bridge_token.empty()) {
      unsetenv("BRIDGE_TOKEN");
    }
#endif
    return rc == 0 ? 0 : 1;
  }

  std::cerr << "Unknown channels command\n";
  return 1;
}

int run_cron(const std::vector<std::string>& args) {
  if (args.size() < 2) {
    std::cerr << "Usage: attoclaw cron <list|add|remove|run|enable> ...\n";
    std::cerr << "Add syntax: attoclaw cron add --name NAME --message MSG [--every SEC | --cron EXPR | --at ISO]\n";
    return 1;
  }

  const fs::path store = get_data_dir() / "cron" / "jobs.json";
  CronService cron(store);

  const std::string sub = args[1];
  if (sub == "list") {
    const bool all = has_flag(args, "--all") || has_flag(args, "-a");
    auto jobs = cron.list_jobs(all);
    if (jobs.empty()) {
      std::cout << "No scheduled jobs.\n";
      return 0;
    }

    for (const auto& j : jobs) {
      std::cout << j.id << "  " << j.name << "  " << j.schedule.kind << "  "
                << (j.enabled ? "enabled" : "disabled") << "\n";
    }
    return 0;
  }

  if (sub == "add") {
    const std::string name = get_flag_value(args, "--name", "job");
    const std::string message = get_flag_value(args, "--message");
    const std::string every_s = get_flag_value(args, "--every");
    const std::string cron_expr = get_flag_value(args, "--cron");
    const std::string at = get_flag_value(args, "--at");

    if (message.empty()) {
      std::cerr << "--message is required\n";
      return 1;
    }

    CronSchedule schedule;
    bool delete_after = false;
    if (!every_s.empty()) {
      schedule.kind = "every";
      schedule.every_ms = std::stoll(every_s) * 1000;
    } else if (!cron_expr.empty()) {
      schedule.kind = "cron";
      schedule.expr = cron_expr;
    } else if (!at.empty()) {
      schedule.kind = "at";
      std::tm tm{};
      std::istringstream ss(at);
      ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
      if (ss.fail()) {
        std::cerr << "Invalid --at format. Use YYYY-MM-DDTHH:MM:SS\n";
        return 1;
      }
      schedule.at_ms = static_cast<int64_t>(std::mktime(&tm)) * 1000;
      delete_after = true;
    } else {
      std::cerr << "Provide --every, --cron, or --at\n";
      return 1;
    }

    auto job = cron.add_job(name, schedule, message, false, "", "", delete_after);
    std::cout << "Added job " << job.id << "\n";
    return 0;
  }

  if (sub == "remove" && args.size() >= 3) {
    const bool ok = cron.remove_job(args[2]);
    std::cout << (ok ? "Removed\n" : "Not found\n");
    return ok ? 0 : 1;
  }

  if (sub == "run" && args.size() >= 3) {
    const bool ok = cron.run_job_now(args[2], has_flag(args, "--force") || has_flag(args, "-f"));
    std::cout << (ok ? "Executed\n" : "Failed\n");
    return ok ? 0 : 1;
  }

  if (sub == "enable" && args.size() >= 3) {
    const bool disable = has_flag(args, "--disable");
    auto job = cron.enable_job(args[2], !disable);
    if (!job.has_value()) {
      std::cout << "Job not found\n";
      return 1;
    }
    std::cout << "Job " << job->id << " " << (disable ? "disabled" : "enabled") << "\n";
    return 0;
  }

  std::cerr << "Unknown cron command\n";
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args;
  args.reserve(static_cast<std::size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }

  if (args.size() <= 1) {
    print_usage();
    return 0;
  }

  const std::string command = args[1];

  if (command == "--version" || command == "-v") {
    std::cout << "attoclaw v0.1.0\n";
    return 0;
  }

  if (command == "onboard") {
    return run_onboard();
  }
  if (command == "status") {
    return run_status();
  }
  if (command == "agent") {
    std::vector<std::string> sub(args.begin() + 2, args.end());
    return run_agent(sub);
  }
  if (command == "gateway") {
    return run_gateway();
  }
  if (command == "channels") {
    std::vector<std::string> sub(args.begin() + 1, args.end());
    return run_channels(sub);
  }
  if (command == "cron") {
    std::vector<std::string> sub(args.begin() + 1, args.end());
    return run_cron(sub);
  }

  print_usage();
  return 1;
}



