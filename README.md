# DDCI - Dominion Distributed Context Intelligence (v1.2.4)

A fast, single-binary AI chat assistant for your terminal, powered by Groq's low-latency inference engine.

DDCI gets you from zero to chatting in under 2 minutes — with sub-millisecond local caching, pre-warmed connection pooling, and seamless containerized execution.

---

## Quick Start (Docker Sandbox - Recommended)

The fastest and cleanest way to run DDCI is via the included `./ddci` launcher, which builds the multi-stage Alpine container and starts a fully sandboxed chat session for you automatically.

### Step 1 - Clone and Launch

```bash
git clone https://github.com/DominionStudios/DDCI.git
cd DDCI
./ddci
```

That's it. The launcher:

- builds the hardened Alpine image (multi-stage, binary-only runtime),
- prompts you for your API key if one isn't configured yet (saved to a local `.env`),
- pins the container to CPU cores `0-1` with a hard 512 MB memory ceiling,
- starts an interactive, read-only-rootfs sandbox with a wiped `/sandbox` on every run.

> **Alternative (declarative):** `docker compose up --build` uses the same hardened `docker-compose.yml` (CPU quota, 512 MB limit, `cap_drop: ALL`, `no-new-privileges`, non-root user, 1.1.1.1 DNS fast-lane).

### Sandbox Hardening At a Glance

| Layer | Guarantee |
| --- | --- |
| Rootfs | `read_only: true` — host filesystem never writable |
| User | Non-root (`uid 1000`, `nologin`, no shell, no home) |
| Capabilities | `cap_drop: ALL` + `no-new-privileges` |
| Memory | Hard 512 MB ceiling; no swap spill (`memory-swap == memory`) |
| CPU | Pinned to dedicated cores (`--cpuset-cpus`, default `0-1`) |
| Personality | `.ddci_vibe` mounted strictly read-only |
| Credentials | `.env` mounted read-only, never baked into the image |
| Scratch | `/sandbox` = 64 MB tmpfs, wiped every session |
| Network | 1.1.1.1 / 1.0.0.1 DNS fast-lane, `timeout:2 attempts:1` |

> **Note for native git users:** inside the container git isn't installed, so workspace-telemetry reports no VCS. Add `git` to the runtime stage if you want repo stats injected.

---

## Alternative: Native Build & Prerequisites

If you prefer to compile and run DDCI natively on your machine without Docker, you need a **C++20 compiler**, **CMake**, **libcurl**, and **SQLite3**.

Pick your OS below to install the dependencies:

### macOS

```bash
xcode-select --install
brew install cmake sqlite3 pkg-config curl
```

### Ubuntu / Debian / Pop!_OS

```bash
sudo apt update && sudo apt install -y build-essential cmake libcurl4-openssl-dev libsqlite3-dev pkg-config
```

### Fedora / RHEL / AlmaLinux

```bash
sudo dnf groupinstall "Development Tools" && sudo dnf install -y cmake libcurl-devel sqlite-devel pkgconfig
```

### Arch Linux / Manjaro

```bash
sudo pacman -S --needed base-devel cmake curl sqlite pkgconf
```

### Alpine Linux

```bash
apk add build-base cmake curl-dev sqlite-dev pkgconf
```

### Windows

The easiest way to run DDCI natively on Windows is through WSL2 (Windows Subsystem for Linux):

```bash
wsl --install
```

Once inside your WSL Ubuntu terminal, run the Ubuntu/Debian command above.

---

## Step-by-Step Native Guide

### Step 1 - Clone the Repository

```bash
git clone https://github.com/DominionStudios/DDCI.git
cd DDCI
```

### Step 2 - Build DDCI

**Option A (Recommended - One-liner):**

```bash
chmod +x install.sh
./install.sh
```

This compiles an optimized build and installs `ddci` directly to your system path.

**Option B (Manual CMake Build):**

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

### Step 3 - Add Your API Key

Get a free Groq key at [console.groq.com/keys](https://console.groq.com/keys), then set it in your terminal:

```bash
export GROQ_API_KEY="gsk_xxxx"
```

*Optional:* want a specific model?

```bash
export MODEL_NAME="llama-3.3-70b-versatile"
```

> **No key? No problem.** Run DDCI anyway — it detects the missing key and walks you through an interactive setup that saves it to a local `.env`.

### Step 4 - Launch and Chat

```bash
# If you installed globally:
ddci

# If you built manually with CMake:
./build/ddci
```

Type your prompt and press **Enter**. Responses stream live to your screen.

---

## Useful Commands

### Startup Flags

| Command | Description |
| --- | --- |
| `ddci --help` / `-h` | Show all CLI options |
| `ddci --version` / `-v` | Display DDCI version (v1.2.4) |
| `ddci --model <name>` / `-m` | Pick a model for this session |
| `ddci --config <path>` / `-c` | Load a custom config file |
| `ddci --set-name "<name>"` | Persist a user name |
| `ddci --get-name` | Print the stored user name |
| `ddci --splash` | Print the DDCI splash art and exit |

### In-Chat Commands (REPL Shortcuts)

| Command | Description |
| --- | --- |
| `/q`, `exit`, or `quit` | Exit the application |
| `/help` | Show in-chat assistance |
| `/clear` | Clear the conversation memory window |
| `/history` | Show recent conversation history |
| `/clear-history` | Wipe the persisted chat history |
| `/name` | Set your display name |
| `/got <prompt>` | Run Graph-of-Thought multi-branch analysis |

---

## License and Copyright

Copyright (c) 2026 Dominion Studios. All rights reserved.

This project is licensed under the **Dominion Studios Source-Available Non-Commercial License** (Academic & Personal Use Only).

**Permitted:** Free to run, inspect, review, modify, and study for professors, researchers, students, educational evaluation, and personal non-commercial projects.

**Restricted:** Commercial distribution, monetization, hosting as a paid service, or integration into revenue-generating products without explicit authorization from Dominion Studios is prohibited.

See the full terms in the [LICENSE](LICENSE) file.
