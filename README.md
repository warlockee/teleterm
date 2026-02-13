# Teleterm

Control your terminal from Telegram. Send keystrokes, read terminal output — all from your phone.

![screenrecording](https://github.com/user-attachments/assets/64602166-6016-4909-bf17-fcabe03002ad)

Works on **macOS** and **Linux**.

> **One bot per machine.** Each machine needs its own Telegram bot token. Create a separate bot for each machine you want to control (e.g. `@my_macbook_bot`, `@my_server_bot`). Only one teleterm instance can use a given bot token at a time.
>

## Quick Start

```bash
git clone https://github.com/warlockee/teleterm.git
cd teleterm
./setup.sh
```

The setup script handles everything: installs dependencies, builds the project, walks you through creating a Telegram bot, and starts teleterm. Works on both macOS and Linux.

## How It Works

On **macOS**, teleterm reads terminal window text via the Accessibility API (`AXUIElement`) and injects keystrokes via `CGEvent`. It works with any terminal app — no Screen Recording permission needed.

On **Linux**, teleterm uses tmux: `tmux list-panes` to discover sessions, `tmux capture-pane` to read content, and `tmux send-keys` to inject keystrokes. All sessions you want to control must run inside tmux.

In both cases, terminal output is sent as monospace text to Telegram with a refresh button to update on demand.

## Manual Setup

### Prerequisites

### Create a Telegram Bot

Each machine needs its own bot. To create one:

1. Open Telegram and message [@BotFather](https://t.me/BotFather)
2. Send `/newbot` and follow the prompts
3. Name it something you'll recognize (e.g. "My Server Terminal")
4. Copy the API token

### Run

```bash
# Save the token (recommended)
echo "YOUR_BOT_TOKEN" > apikey.txt
./teleterm

# Or pass it directly
./teleterm --apikey YOUR_BOT_TOKEN
```

### Options

| Flag | Description |
|------|-------------|
| `--apikey <token>` | Telegram bot API token |
| `--use-weak-security` | Disable Authenticator (owner-only lock still applies) |
| `--dbfile <path>` | Custom database path (default: `./mybot.sqlite`) |
| `--dangerously-attach-to-any-window` | Show all windows, not just terminals (macOS only) |

## Usage

Message your bot on Telegram:

| Command | Action |
|---------|--------|
| `.list` | List available terminal sessions |
| `.1` `.2` ... | Connect to a session by number |
| `.help` | Show help |
| `.otptimeout <seconds>` | Set TOTP session timeout |
| Any other text | Sent as keystrokes to the connected terminal |

### Linux: tmux requirement

On Linux, teleterm controls tmux sessions. Make sure your work is running inside tmux:

```bash
# Start a named session
tmux new -s dev

# Or start detached sessions
tmux new -s server1 -d
tmux new -s server2 -d
```

Then run teleterm separately (outside tmux or in its own tmux window) and use `.list` to see your sessions.

### Keystroke Modifiers

Prefix your message with an emoji to add a modifier key:

| Emoji | Modifier | Example |
|-------|----------|---------|
| `❤️` | Ctrl | `❤️c` = Ctrl+C |
| `💙` | Alt | `💙x` = Alt+X |
| `💚` | Cmd | macOS only |
| `💛` | ESC | `💛` = send Escape |
| `🧡` | Enter | `🧡` = send Enter |
| `💜` | Suppress auto-newline | `ls -la💜` = no Enter appended |

### Escape Sequences

`\n` for Enter, `\t` for Tab, `\\` for literal backslash.

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `TELETERM_VISIBLE_LINES` | `40` | Number of terminal lines to include in output. Increase for more context, decrease for shorter messages. |
| `TELETERM_SPLIT_MESSAGES` | off | When set to `1` or `true`, long output is split across multiple Telegram messages. When off (default), output is truncated to fit a single message, keeping the most recent lines. |

Terminal output is sent as a single message by default. Each new command or refresh **deletes the previous output messages** and sends fresh ones, creating a clean "live terminal" view rather than spamming the chat.

If your terminal produces very long output (e.g. build logs) and you want to see all of it, enable splitting:

```bash
TELETERM_SPLIT_MESSAGES=1 ./teleterm
```

## Security

- **Owner lock**: The first Telegram user to message the bot becomes the owner. All other users are ignored.
- **TOTP**: By default, teleterm requires Google Authenticator verification. A QR code is shown on first launch. Use `--use-weak-security` to disable.
- **One bot = one machine**: Don't share a bot token across machines. Each machine should have its own bot.
- **Reset**: Delete `mybot.sqlite` to reset ownership and TOTP.

## Permissions

**macOS:** Requires Accessibility permission. macOS will prompt on first use, or grant it in System Settings > Privacy & Security > Accessibility.

**Linux:** No special permissions needed. Just ensure the user running teleterm can access the tmux socket.

## Supported Terminals

**macOS:** Terminal.app, iTerm2, Ghostty, kitty, Alacritty, Hyper, Warp, WezTerm, Tabby.

**Linux:** Any terminal running inside tmux.

## License

MIT — see [LICENSE](LICENSE).

Based on [tgterm](https://github.com/antirez/tgterm) by Salvatore Sanfilippo (antirez).
