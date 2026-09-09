# DDCI - Deterministic Distributed Context Intelligence

A fast, single-binary AI chat assistant for your terminal, powered by Groq's low-latency inference engine.

DDCI gets you from zero to chatting in under 2 minutes with sub-millisecond local caching and pre-warmed connection pooling.

---

## Prerequisites

You only need four basic tools: a C++20 compiler, CMake, libcurl, and SQLite3.

Pick your terminal or OS below and run the setup command:

### macOS
xcode-select --install
brew install cmake sqlite3 pkg-config curl

### Ubuntu / Debian / Pop!_OS
sudo apt update && sudo apt install -y build-essential cmake libcurl4-openssl-dev libsqlite3-dev pkg-config

### Fedora / RHEL / AlmaLinux
sudo dnf groupinstall "Development Tools" && sudo dnf install -y cmake libcurl-devel sqlite-devel pkgconfig

### Arch Linux / Manjaro
sudo pacman -S --needed base-devel cmake curl sqlite pkgconf

### Alpine Linux
apk add build-base cmake curl-dev sqlite-dev pkgconf

### Windows
The easiest way to run DDCI on Windows is through WSL2 (Windows Subsystem for Linux):
wsl --install
Once inside your WSL Ubuntu terminal, run the Ubuntu/Debian command above.

---

## Step-by-Step Guide

### Step 1 - Clone the Repository
Open your terminal and grab the code:
git clone https://github.com/DominionStudios/DDCI.git
cd DDCI

### Step 2 - Build DDCI

Option A (Recommended - One-liner):
chmod +x install.sh
./install.sh
This compiles an optimized build and installs ddci directly to your system path.

Option B (Manual CMake Build):
cmake -S . -B build
cmake --build build --parallel

---

### Step 3 - Add Your API Key

1. Get a free Groq key at https://console.groq.com/keys
2. Set it in your terminal:

export GROQ_API_KEY="gsk_xxxx"

Optional: Want a specific model?
export MODEL_NAME="llama-3.3-70b-versatile"

Note: If you do not have a key set, run DDCI anyway. It will detect the missing key and guide you through an interactive setup screen to create and save one to a local .env file.

---

### Step 4 - Launch and Chat

If you used ./install.sh, run:
ddci

If you built manually with CMake, run:
./build/ddci

Type your prompt and press Enter. Responses will stream live to your screen.

---

## Useful Commands

### Startup Flags (Run in Terminal)

| Command | Description |
|---|---|
| ddci --help / -h | Show all CLI options |
| ddci --version / -v | Display DDCI version |
| ddci --model <name> / -m | Pick a model for this session |
| ddci --config <path> / -c | Load a custom config file |

### In-Chat Commands (REPL Shortcuts)

| Command | Description |
|---|---|
| /q or exit or quit | Exit the application |
| /help | Show in-chat assistance |
| /clear | Clear conversation memory window |
| /got <prompt> | Run Graph-of-Thought multi-branch analysis |

---

## License and Copyright

Copyright (c) 2026 Dominion Studios. All rights reserved.

This project is licensed under the Dominion Studios Source-Available Non-Commercial License (Academic & Personal Use Only).

* Permitted: Free to run, inspect, review, modify, and study for professors, researchers, students, educational evaluation, and personal non-commercial projects.
* Restricted: Commercial distribution, monetization, hosting as a paid service, or integration into revenue-generating products without explicit authorization from Dominion Studios is prohibited.

See the full terms in the LICENSE file.