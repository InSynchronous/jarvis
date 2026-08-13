# jarvis

A terminal-based AI assistant with MCP (Model Context Protocol) support, voice input via Whisper, and agentic tool-use workflows. Written in C++.

## Features

- **MCP client** - Connects to external tool servers (filesystem, search, etc.) via stdio or HTTP
- **Voice input** - Real-time speech-to-text using Whisper.cpp (local, no cloud)
- **Multiple modes** - Coder, Pentester, Planner personas with different system prompts
- **Tool use** - LLM can invoke MCP tools, chain calls, and iterate
- **Context management** - Sliding window with optional summarization to preserve long conversations
- **Model-agnostic** - Works with any OpenAI-compatible API (OpenRouter, OpenCode Zen, local Ollama, etc.)

## Build

```bash
git submodule update --init --recursive
cmake -B build
cmake --build build
```

## Models

Download a Whisper model for voice input:

```bash
sh ./external/whisper.cpp/models/download-ggml-model.sh base.en
cp external/whisper.cpp/models/ggml-base.en.bin models/
```

## Configuration

Create `.env` in the project root:

```ini
api_key=your-api-key-here
JARVIS_ENDPOINT=https://opencode.ai/zen/v1/chat/completions
JARVIS_MODEL=deepseek-v4-flash-free
EXA_KEY=your-exa-key-here
```

- `api_key` - API key for the LLM endpoint (OpenRouter, OpenCode Zen, etc.)
- `JARVIS_ENDPOINT` - LLM endpoint URL (defaults to OpenCode Zen if unset)
- `JARVIS_MODEL` - Default model ID (a model passed on the CLI overrides this)
- `EXA_KEY` - API key for Exa search (used by the Exa MCP server)

### Google Workspace (Gmail / Drive / Calendar)

Gmail is served by a **local MCP server** (`mcp/gmail_server.py`) that calls the
Gmail REST API directly with an OAuth bearer token — no Google MCP gateway, no
Developer Preview enrollment needed. Drive and Calendar still use Google's
`drivemcp.googleapis.com` / `calendarmcp.googleapis.com` MCP servers, which are
Developer Preview only. All three share the same OAuth token setup below; tokens
are stored under `mcp/tokens/` (gitignored) and refreshed by the helper script.

One-time setup:

1. Enable the base APIs in the Google Cloud console. Only `gmail.googleapis.com`
   is required for the Gmail server; `drive.googleapis.com` / `calendar-json.googleapis.com`
   are needed for Drive/Calendar:
   ```bash
   gcloud services enable gmail.googleapis.com drive.googleapis.com calendar-json.googleapis.com
   ```
2. Configure the OAuth consent screen, adding the scopes you want (see below).
   For a **Workspace (business/school) account** the OAuth app must also be marked
   **Trusted** in the Google Admin console (Apps → OAuth app access), otherwise
   restricted scopes are blocked. For a personal Gmail account this step is not needed.
3. Create an OAuth **Web application** client and register the redirect URI:
   `http://127.0.0.1:8342/oauth/callback`
4. Put the client ID/secret in `.env`:
   ```ini
   GOOGLE_CLIENT_ID=your-client-id.apps.googleusercontent.com
   GOOGLE_CLIENT_SECRET=your-client-secret
   ```
5. Log in for each server you want:
   ```bash
   python3 mcp/google_auth.py login gmail
   python3 mcp/google_auth.py login drive
   python3 mcp/google_auth.py login calendar
   ```

Access tokens expire after ~1 hour. The gmail server auto-refreshes before each
call; refresh other servers manually:

```bash
python3 mcp/google_auth.py refresh gmail
```

Other commands: `status <server>`, `logout <server>`.

If a Google server has no token file, jarvis prints a hint and continues; a failed
server does not prevent the rest from connecting.

#### Gmail MCP tools

The local `gmail` server exposes: `get_profile`, `search_threads`, `get_thread`,
`get_message`, `list_labels`, and `create_draft`. The token's `gmail.readonly` +
`gmail.compose` scopes cover these. Sending mail is **not** included; to add it,
grant the `gmail.send` scope and re-login:

```bash
python3 mcp/google_auth.py login gmail --scopes \
    https://www.googleapis.com/auth/gmail.readonly \
    https://www.googleapis.com/auth/gmail.compose \
    https://www.googleapis.com/auth/gmail.send
```

### Troubleshooting: "The caller does not have permission"

This affects only the Drive/Calendar (Google MCP) servers, or a Gmail server still
pointing at `gmailmcp.googleapis.com`. Check, in order:

1. **Developer Preview Program enrollment** — https://developers.google.com/workspace/preview
   must show your account/project as a participant.
2. **MCP service enabled** — confirm `drivemcp.googleapis.com` / `calendarmcp.googleapis.com`
   show up under "APIs & Services → Enabled APIs".
3. **Admin trust gate** — for Workspace org accounts, the OAuth client must be marked
   Trusted in the Admin console for restricted scopes (personal Gmail: not applicable).
4. Sanity-check the token with `python3 mcp/google_auth.py status gmail` — if it is
   valid, the problem is in steps 1–3, not the harness.

The LLM endpoint and default model are configured via `JARVIS_ENDPOINT` and
`JARVIS_MODEL` in `.env` (see above). Any OpenAI-compatible endpoint works,
e.g. OpenCode Zen, OpenRouter, or a local Ollama server.

## Running

```bash
./build/jarvis [folder] [model] [options]
```

Arguments:
- `folder` — Project directory to work in (default: current directory)
- `model` — Model ID (default: `openai/gpt-oss-20b`)

Options:
- `-m, --mode <mode>` — Agent mode: `code`, `hack`, `plan` (default: `code`)
- `-h, --help` — Show help

Examples:
```bash
./build/jarvis
./build/jarvis ~/my-project
./build/jarvis ~/my-project --mode plan
./build/jarvis ~/my-project nvidia/nemotron-3-super:free
```

## Usage

Once running, type naturally. Commands:
- `/voice` — Start voice input (hold to talk, release to send)
- `/model` — Show or switch model (e.g., `/model openai/gpt-4o`)
- `/servers` — List connected MCP servers and available tools
- `/mode <code|hack|plan>` — Switch agent persona

The assistant will use MCP tools automatically when needed. Tool calls and results are shown inline.

## Project Structure

```
src/
  main.cpp          # Entry point, CLI parsing, main loop
  Ollama.cpp        # LLM client, context management, streaming
  MCPManager.cpp    # MCP client, server lifecycle, tool calls
  HTTPTransport.cpp # HTTP transport for MCP servers
  Microphone.cpp    # Audio capture (PortAudio)
  Whisper.cpp       # Whisper.cpp integration
mcp/
  server.py         # Filesystem MCP server (stdio)
  exa.py            # Exa search MCP server (stdio)
  code.md           # Coder system prompt
  hack.md           # Pentester system prompt
  plan.md           # Planner system prompt
  tools.md          # Tool descriptions injected into prompts
```

## Dependencies

- C++20 compiler
- CMake 3.20+
- PortAudio (for microphone input)
- curl (HTTP)
- nlohmann/json (included as submodule)
- Whisper.cpp (included as submodule)
- termmark (included as submodule)
