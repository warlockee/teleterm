#!/bin/bash
set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
BOLD='\033[1m'
NC='\033[0m'

info()  { echo -e "${BLUE}=>${NC} $1"; }
ok()    { echo -e "${GREEN}=>${NC} $1"; }
warn()  { echo -e "${YELLOW}=>${NC} $1"; }
err()   { echo -e "${RED}=>${NC} $1"; }

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo -e "${BOLD}"
echo "  _       _      _                      "
echo " | |_ ___| | ___| |_ ___ _ __ _ __ ___  "
echo " | __/ _ \ |/ _ \ __/ _ \ '__| '_ \` _ \ "
echo " | ||  __/ |  __/ ||  __/ |  | | | | | |"
echo "  \__\___|_|\___|\__\___|_|  |_| |_| |_|"
echo ""
echo -e "${NC}  Control your Mac terminal from Telegram"
echo ""

# Step 1: Check Xcode CLI tools
info "Checking for Xcode Command Line Tools..."
if ! xcode-select -p &>/dev/null; then
    warn "Xcode Command Line Tools not found. Installing..."
    xcode-select --install
    echo ""
    echo "Please complete the installation dialog, then re-run this script."
    exit 1
fi
ok "Xcode Command Line Tools found."

# Step 2: Build
info "Building teleterm..."
make clean -s 2>/dev/null || true
if make -s 2>&1 | grep -v "warning:"; then
    ok "Build successful."
else
    err "Build failed. Check errors above."
    exit 1
fi

# Step 3: API key setup
echo ""
if [ -f apikey.txt ]; then
    EXISTING_KEY=$(cat apikey.txt | tr -d '[:space:]')
    ok "Found existing API key in apikey.txt"
    echo -e "   Current key: ${BOLD}${EXISTING_KEY:0:10}...${NC}"
    echo ""
    read -p "   Keep this key? [Y/n] " -n 1 -r
    echo ""
    if [[ $REPLY =~ ^[Nn]$ ]]; then
        rm apikey.txt
    fi
fi

if [ ! -f apikey.txt ]; then
    echo -e "${BOLD}Telegram Bot Setup${NC}"
    echo ""
    echo "  You need a Telegram bot token. To create one:"
    echo ""
    echo "  1. Open Telegram and message @BotFather"
    echo "  2. Send /newbot"
    echo "  3. Choose a name and username for your bot"
    echo "  4. Copy the API token"
    echo ""
    read -p "  Paste your bot API token: " API_KEY
    API_KEY=$(echo "$API_KEY" | tr -d '[:space:]')

    if [ -z "$API_KEY" ]; then
        err "No API key provided."
        exit 1
    fi

    # Validate the token
    info "Validating token..."
    RESPONSE=$(curl -s "https://api.telegram.org/bot${API_KEY}/getMe")
    if echo "$RESPONSE" | grep -q '"ok":true'; then
        BOT_USERNAME=$(echo "$RESPONSE" | grep -o '"username":"[^"]*"' | cut -d'"' -f4)
        ok "Token valid! Bot: @${BOT_USERNAME}"
        echo "$API_KEY" > apikey.txt
    else
        err "Invalid token. Please check and try again."
        exit 1
    fi
fi

# Step 4: Security mode
echo ""
echo -e "${BOLD}Security Mode${NC}"
echo ""
echo "  teleterm supports TOTP authentication (Google Authenticator)."
echo "  This requires scanning a QR code on first launch."
echo ""
echo "  1) Enable TOTP  (recommended for shared networks)"
echo "  2) Skip TOTP    (simpler, owner-only lock still applies)"
echo ""
read -p "  Choose [1/2]: " -n 1 -r SECURITY_CHOICE
echo ""

EXTRA_FLAGS=""
if [[ "$SECURITY_CHOICE" == "2" ]]; then
    EXTRA_FLAGS="--use-weak-security"
    ok "TOTP disabled. First Telegram user to message becomes owner."
else
    ok "TOTP enabled. You'll scan a QR code on first launch."
fi

# Step 5: Accessibility permission check
echo ""
info "Checking Accessibility permission..."
echo ""
echo "  teleterm needs Accessibility permission to:"
echo "  - Read terminal window text"
echo "  - Send keystrokes to terminal windows"
echo "  - List and focus terminal windows"
echo ""
echo "  If prompted by macOS, click 'Allow' or add your terminal app"
echo "  (iTerm2, Terminal, etc.) in:"
echo "  System Settings > Privacy & Security > Accessibility"
echo ""

# Step 6: Create launch script
cat > run.sh << RUNEOF
#!/bin/bash
cd "\$(dirname "\$0")"
exec ./teleterm $EXTRA_FLAGS "\$@"
RUNEOF
chmod +x run.sh
ok "Created run.sh"

# Step 7: Summary
echo ""
echo -e "${GREEN}${BOLD}Setup complete!${NC}"
echo ""
echo "  To start teleterm:"
echo ""
echo -e "    ${BOLD}cd $SCRIPT_DIR && ./run.sh${NC}"
echo ""
echo "  Then open Telegram and message your bot."
echo "  The first user to message becomes the owner."
echo ""
echo "  Bot commands:"
echo "    .list    - List terminal windows"
echo "    .1 .2 .. - Connect to a window"
echo "    .help    - Show all commands"
echo ""
echo -e "  ${YELLOW}Tip:${NC} To run in the background:"
echo -e "    ${BOLD}nohup ./run.sh &${NC}"
echo ""

read -p "  Start teleterm now? [Y/n] " -n 1 -r
echo ""
if [[ ! $REPLY =~ ^[Nn]$ ]]; then
    echo ""
    info "Starting teleterm..."
    exec ./teleterm $EXTRA_FLAGS
fi
