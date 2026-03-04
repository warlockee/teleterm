# teleterm
### Control your terminal from anywhere.

---

## The Problem

Developers and engineers are increasingly relying on autonomous AI agents (Claude Code, Codex CLI, Aider, Cline) to write code, run builds, and manage infrastructure. These agents run for minutes to hours in terminal sessions.

**But you can't watch your terminal all day.**

- You step away from your desk. Your AI agent hits a prompt. It waits. You lose 30 minutes.
- A deploy is running on a remote server. You need to check it from your phone. You can't.
- Your build breaks at 2am. You need to send one command. You open your laptop, VPN in, SSH, navigate to the right directory... 10 minutes for a 5-second fix.

**There is no good way to monitor and interact with running terminal sessions from a mobile device.**

SSH apps exist but they create new sessions -- they can't attach to what's already running. Slack bots run pre-defined commands -- they can't give you raw terminal access. Desktop tools require your laptop.

---

## The Solution

**teleterm** turns Telegram into a remote terminal controller.

It reads what's on your terminal screen and sends it to you. You type back, and it injects keystrokes into the actual running session. No new shell. No SSH. You're interacting with the real terminal, exactly as if you were sitting in front of it.

```
You (phone)  -->  Telegram  -->  teleterm  -->  Your actual terminal
                                                (iTerm2, tmux, kitty, etc.)
```

**Key capabilities:**
- **List** all terminal windows/sessions on your machine
- **Read** the current output of any terminal
- **Send** keystrokes, commands, Ctrl+C, Enter -- anything
- **AI Manager** -- an autonomous agent that monitors terminals, executes tasks, answers questions about what's running, and remembers your preferences across sessions

---

## How It Works

### For the user (30 seconds to set up):

```bash
curl -fsSL https://raw.githubusercontent.com/warlockee/teleterm/main/setup.sh | bash
```

Setup walks you through:
1. Building from source (C, zero heavy dependencies)
2. Creating a Telegram bot via @BotFather
3. Optionally enabling the AI manager (Claude-powered)
4. Optionally enabling TOTP (Google Authenticator)

### Daily workflow:

```
You: .list
Bot: Terminal 1 [12399]: iTerm2 -- ~/project
     Terminal 2 [12400]: iTerm2 -- npm run dev

You: .1
Bot: [Terminal output appears as monospace text + Refresh button]

You: git status
Bot: [Shows git output in real time]

You: .mgr
You: "check if the build finished in terminal 2, and if it failed, show me the errors"
Bot: I'll check terminal 2 now...
Bot: Build failed with 3 errors. Here are the relevant lines: [output]
```

---

## The AI Manager

The optional AI manager turns teleterm from a remote control into an **autonomous terminal agent**.

| Capability | Description |
|---|---|
| **Command execution** | Send any command to any terminal, with automatic queueing per-terminal |
| **Output monitoring** | Watches for output to stabilize before reporting back (no guessing fast vs slow) |
| **Background tasks** | "Watch terminal 3 and tell me when the deploy finishes" |
| **Risk classification** | Auto-executes safe prompts, asks for confirmation on dangerous ones (rm -rf, production, force push) |
| **Long-term memory** | Remembers user rules, preferences, and environment facts across restarts (SQLite-backed) |

The AI manager uses Claude via tool-use. It has 6 tools for terminal operations and 2 for persistent memory. Every command is queued per-terminal to prevent overlap. Output stability detection (not regex) determines when a command finishes.

---

## Market

### The developer tooling market is $45B+ and growing fast.

Three converging trends create teleterm's opportunity:

**1. AI agents are taking over terminals**
- Claude Code, Codex CLI, Gemini CLI, Cline, Aider -- all run autonomously in terminal sessions
- These agents need human oversight: approval prompts, error recovery, progress monitoring
- The "human in the loop" is currently required to sit at their desk

**2. Remote/async work is the norm**
- 70%+ of developers work remotely at least part-time
- Mobile-first access to dev infrastructure is expected, not optional
- Engineers manage servers, CI/CD, and deploys from everywhere

**3. Messaging platforms are the universal interface**
- Telegram has 900M+ monthly active users
- Bots are a first-class Telegram feature with rich APIs
- No app to install -- just message a bot from any device

### TAM/SAM/SOM

| Segment | Size | Rationale |
|---|---|---|
| **TAM** | $45B | Global developer tools market (2025) |
| **SAM** | $2.5B | Remote development / mobile DevOps tools |
| **SOM** | $50M | Individual developers and small teams using AI terminal agents who need mobile access |

---

## Competitive Landscape

**No one else does what teleterm does.**

| | teleterm | tgterm (antirez) | Shell bots | SSH apps | Warp / AI terminals | ChatOps (PagerDuty) |
|---|---|---|---|---|---|---|
| Attach to existing sessions | Yes | Yes | No | No | N/A | No |
| Mobile-first | Yes | Yes | Yes | Partial | No | Yes (Slack) |
| macOS GUI terminals | Yes | Yes | No | No | Desktop only | No |
| Linux/tmux | Yes | No | Some | Yes | No | Some |
| AI agent built in | Yes | No | No | No | Yes (desktop) | Partial |
| TOTP security | Yes | No | Rare | Yes | N/A | Enterprise |
| Long-term memory | Yes | No | No | No | No | No |
| Self-hosted / free | Yes | Yes | Yes | Paid | Freemium | Paid |
| Command queueing | Yes | No | No | No | N/A | Yes |

**Closest competitor**: antirez's tgterm (macOS-only, no Linux, no AI, no security, no memory). teleterm is a fork that has diverged significantly.

**Key insight**: AI terminal tools (Warp, Claude Code, Cline) are **complementary** -- they run inside the terminals that teleterm controls. We're the remote access layer for the AI coding agent era.

---

## Technology

### Architecture

```
Telegram  <-->  teleterm (C)  <-->  terminal sessions
                    |
                    |--- teleterm-ctl (C CLI)
                    |
                    +--- teleterm-mgr (Python + Claude API)
                              |
                              +--- memory.sqlite (persistent)
```

### By the numbers

| Metric | Value |
|---|---|
| Core C code | ~5,500 lines |
| Third-party C (cJSON, QR, SDS, SHA1) | ~5,000 lines |
| Python AI manager | ~1,000 lines |
| External dependencies | libcurl, libsqlite3, tmux (Linux) |
| Build time | < 5 seconds |
| Binary size | < 500KB |
| Memory footprint | < 10MB |
| Platforms | macOS 14+, Linux (Debian/Ubuntu/RHEL) |

### Technical moat

1. **Native OS integration** -- macOS Accessibility API for reading terminal windows (no screen capture, no OCR). Direct CGEvent injection for keystrokes. No electron, no overhead.
2. **Real terminal attachment** -- not a new shell. Connects to your actual iTerm2, kitty, Ghostty, Alacritty, Terminal.app windows.
3. **Stability-based command detection** -- no regex patterns, no heuristics. Monitors output and waits until it stops changing. Works with any program.
4. **Per-terminal command queue** -- prevents overlapping commands. AI manager can fire off multiple commands and they execute serially, each waiting for the previous to finish.
5. **Persistent AI memory** -- the manager learns your preferences and environment across restarts. No other terminal tool has this.

---

## Traction

- Open source on GitHub ([warlockee/teleterm](https://github.com/warlockee/teleterm))
- Fork of antirez's tgterm (Salvatore Sanfilippo, creator of Redis)
- Curl one-liner install working on macOS and Linux
- 7 merged PRs with active development
- AI manager with 8 tools, background tasks, risk classification, and persistent memory
- Supported terminal apps: iTerm2, Terminal.app, Ghostty, kitty, Alacritty, Warp, and all tmux sessions on Linux

---

## Business Model

### Phase 1: Open Source Growth (Now)
- Free, MIT-licensed core product
- Build community, get feedback, iterate fast
- Establish teleterm as the standard for remote terminal access

### Phase 2: Managed Service
- **teleterm Cloud** -- hosted relay that eliminates self-hosting
- Install an agent on your machine, get a Telegram bot instantly
- Free tier (1 machine, 100 commands/day) + Pro tier ($9/mo, unlimited)
- Team tier ($29/mo/seat) with shared terminal access and audit logs

### Phase 3: Platform
- **Plugin marketplace** -- community-built tools for the AI manager (deploy helpers, monitoring, CI/CD integration)
- **Enterprise** -- SSO, RBAC, compliance logging, Slack/Teams integration
- **API** -- third-party apps can control terminals via teleterm's infrastructure

---

## The Ask

**Raising $1.5M seed to:**

| Use | Allocation |
|---|---|
| Engineering (2 hires) | 60% |
| Infrastructure (teleterm Cloud) | 20% |
| Community & marketing | 15% |
| Legal & ops | 5% |

### 18-month milestones

1. **Month 3** -- teleterm Cloud beta (hosted relay, zero-setup install)
2. **Month 6** -- 5,000 GitHub stars, 1,000 active installations
3. **Month 9** -- Pro tier launch, first revenue
4. **Month 12** -- Team tier, Windows support, Slack/Discord integration
5. **Month 18** -- 500 paying customers, $50K MRR, Series A ready

---

## Why Now

1. **AI agents are exploding** -- Claude Code, Codex CLI, and Cline launched in the last 12 months. Millions of developers now run autonomous agents in their terminals. They all need remote monitoring.

2. **antirez validated the concept** -- the creator of Redis built tgterm because he needed it personally. When the Redis creator builds a tool for himself, there's a real problem here.

3. **Mobile-first dev tooling is inevitable** -- every other industry went mobile-first years ago. Development is catching up. The developer who can approve a deploy from their phone while walking the dog will outperform the one who has to open their laptop.

4. **Telegram is the right platform** -- 900M users, excellent bot API, works on every device, no app approval process. Telegram bots are the fastest way to put a developer tool on someone's phone.

---

## Team

Building teleterm: experienced systems programmers with deep knowledge of macOS internals, terminal emulators, and LLM agent architectures.

- C systems programming (macOS Accessibility API, CGEvent, POSIX)
- Python AI agent development (Claude API tool-use, async patterns)
- DevOps and infrastructure automation
- Open source community building

---

*teleterm -- the missing remote control for the AI agent era.*

*Contact: [your-email] | GitHub: github.com/warlockee/teleterm*
