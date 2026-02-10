# teleterm

Control your Mac terminal from Telegram. Send keystrokes, read terminal output — all from your phone.

Fork of [antirez/tgterm](https://github.com/antirez/tgterm) with screenshot capture replaced by text-based output via the macOS Accessibility API. No Screen Recording permission needed.

## Quick Start

```bash
git clone https://github.com/warlockee/teleterm.git
cd teleterm
./setup.sh
```

The setup script will:
- Build the project
- Walk you through creating a Telegram bot
- Save your API key
- Configure security mode
- Start teleterm

## Manual Setup

### Prerequisites

- macOS 14+
- Xcode Command Line Tools (`xcode-select --install`)

### Build

```bash
make
```

### Create a Telegram Bot

1. Open Telegram and message [@BotFather](https://t.me/BotFather)
2. Send `/newbot` and follow the prompts
3. Copy the API token

### Run

```bash
# Pass the API key directly
./teleterm --apikey YOUR_BOT_TOKEN

# Or save it to a file (recommended)
echo "YOUR_BOT_TOKEN" > apikey.txt
./teleterm
```

### Options

| Flag | Description |
|------|-------------|
| `--apikey <token>` | Telegram bot API token |
| `--use-weak-security` | Disable TOTP authentication (owner-only lock still applies) |
| `--dbfile <path>` | Custom database path (default: `./mybot.sqlite`) |
| `--dangerously-attach-to-any-window` | Show all windows, not just terminals |

## Usage

Once teleterm is running, message your bot on Telegram:

| Command | Action |
|---------|--------|
| `.list` | List available terminal windows |
| `.1` `.2` ... | Connect to a window by number |
| `.help` | Show help |
| `.otptimeout <seconds>` | Set TOTP session timeout |
| Any other text | Sent as keystrokes to the connected terminal |

### Keystroke Modifiers

| Emoji | Modifier |
|-------|----------|
| `❤️` | Ctrl |
| `💙` | Alt |
| `💚` | Cmd |
| `💛` | ESC |
| `🧡` | Enter |
| `💜` | Suppress auto-newline |

Example: send `❤️c` to send Ctrl+C.

### Escape Sequences

`\n` for Enter, `\t` for Tab, `\\` for backslash.

## Security

- **Owner lock**: The first Telegram user to message the bot becomes the owner. All other users are ignored.
- **TOTP**: By default, teleterm requires Google Authenticator verification. A QR code is shown on first launch. Use `--use-weak-security` to disable.
- **Reset**: Delete `mybot.sqlite` to reset ownership and TOTP.

## Permissions

teleterm requires **Accessibility** permission to read terminal text and inject keystrokes. macOS will prompt you on first use, or grant it manually in:

**System Settings > Privacy & Security > Accessibility**

Add your terminal app (iTerm2, Terminal, etc.) or teleterm itself.

## Supported Terminals

Terminal.app, iTerm2, Ghostty, kitty, Alacritty, Hyper, Warp, WezTerm, Tabby.

## How It Works

Unlike the original tgterm which captures window screenshots (requiring Screen Recording permission), teleterm reads terminal text content via the macOS Accessibility API (`AXUIElement`). The visible terminal output is sent as monospace text messages to Telegram, with a refresh button to update on demand.

## License

MIT — see [LICENSE](LICENSE).

Based on [tgterm](https://github.com/antirez/tgterm) by Salvatore Sanfilippo (antirez).
