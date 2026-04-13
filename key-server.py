"""
keeper-key-server.py — Local HTTP API for brothers-keeper vault

Bare-metal GPU agents request API keys from the keeper.
The keeper holds secrets. Agents authenticate with a personal token.
Budget is tracked per agent.

Architecture:
  Agent (no secrets) --HTTP--> Keeper (holds keys, tracks budget)
                              |
                              v
                         Provider API

Endpoints:
  POST /auth           — agent authenticates, gets session token
  POST /key/:provider  — request API key (authenticated)
  GET  /budget/:agent  — check remaining budget
  GET  /status         — keeper health + active agents

Security:
  - Localhost only (127.0.0.1)
  - Agent auth tokens are HMAC of agent_name + shared_secret
  - No keys ever leave the local machine
  - Budget enforced per-agent per-day
"""

import json
import hashlib
import hmac
import time
import os
import sys
import argparse
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse

# ═══ Config ═══

DEFAULT_PORT = 9437  # "KEEP" on phone keypad
DEFAULT_VAULT_PATH = os.path.expanduser("~/.local/share/brothers-keeper/vault.json")
DEFAULT_ALLOWANCES_PATH = os.path.expanduser("~/.local/share/brothers-keeper/allowances.json")
DEFAULT_SHARED_SECRET_PATH = os.path.expanduser("~/.local/share/brothers-keeper/keeper.secret")

# ═══ State ═══

vault = {}          # provider -> key
allowances = {}     # agent_name -> {daily_limit_usd, used_today_usd, reset_at, calls}
sessions = {}       # session_token -> {agent_name, expires_at}
shared_secret = ""  # HMAC secret for agent auth tokens


def load_vault(path):
    global vault
    if os.path.exists(path):
        with open(path) as f:
            vault = json.load(f)


def save_vault(path):
    with open(path, 'w') as f:
        json.dump(vault, f, indent=2)


def load_allowances(path):
    global allowances
    if os.path.exists(path):
        with open(path) as f:
            allowances = json.load(f)
        # Reset daily counters if expired
        now = time.time()
        for agent, a in allowances.items():
            if now > a.get('reset_at', 0):
                a['used_today_usd'] = 0.0
                a['calls'] = 0
                a['reset_at'] = now + 86400


def save_allowances(path):
    with open(path, 'w') as f:
        json.dump(allowances, f, indent=2)


def load_secret(path):
    global shared_secret
    if os.path.exists(path):
        with open(path) as f:
            shared_secret = f.read().strip()
    else:
        # Generate new secret
        shared_secret = hashlib.sha256(os.urandom(32)).hexdigest()
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'w') as f:
            f.write(shared_secret)


def make_agent_token(agent_name):
    """Generate auth token for an agent."""
    msg = f"{agent_name}:{int(time.time() / 3600)}"  # hourly rotation
    return hmac.new(shared_secret.encode(), msg.encode(), hashlib.sha256).hexdigest()[:32]


def verify_agent_token(token, agent_name):
    """Verify an agent's auth token."""
    # Check current and previous hour (handle rotation)
    for hour_offset in [0, -1]:
        msg = f"{agent_name}:{int(time.time() / 3600) + hour_offset}"
        expected = hmac.new(shared_secret.encode(), msg.encode(), hashlib.sha256).hexdigest()[:32]
        if hmac.compare_digest(token, expected):
            return True
    return False


def check_budget(agent_name, estimated_cost_usd):
    """Check if agent has budget remaining. Returns (ok, message)."""
    if agent_name not in allowances:
        # Auto-register with default budget
        allowances[agent_name] = {
            'daily_limit_usd': 1.0,  # $1/day default
            'used_today_usd': 0.0,
            'calls': 0,
            'reset_at': time.time() + 86400
        }
        save_allowances(DEFAULT_ALLOWANCES_PATH)

    a = allowances[agent_name]

    # Reset if expired
    if time.time() > a.get('reset_at', 0):
        a['used_today_usd'] = 0.0
        a['calls'] = 0
        a['reset_at'] = time.time() + 86400

    remaining = a['daily_limit_usd'] - a['used_today_usd']
    if estimated_cost_usd > remaining:
        return False, f"Budget exceeded: ${remaining:.4f} remaining, ${estimated_cost_usd:.4f} requested"

    return True, f"${remaining:.4f} remaining"


def record_usage(agent_name, cost_usd, provider):
    """Record API usage for budget tracking."""
    if agent_name in allowances:
        allowances[agent_name]['used_today_usd'] += cost_usd
        allowances[agent_name]['calls'] += 1
        save_allowances(DEFAULT_ALLOWANCES_PATH)

        # Log
        log_dir = os.path.expanduser("~/.local/share/brothers-keeper/logs")
        os.makedirs(log_dir, exist_ok=True)
        log_path = os.path.join(log_dir, "token_usage.log")
        with open(log_path, 'a') as f:
            f.write(f"{int(time.time())} {agent_name} {provider} ${cost_usd:.6f}\n")


# ═══ HTTP Handler ═══

class KeeperHandler(BaseHTTPRequestHandler):

    def log_message(self, format, *args):
        """Suppress default logging."""
        pass

    def _send_json(self, data, status=200):
        body = json.dumps(data).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _read_body(self):
        length = int(self.headers.get('Content-Length', 0))
        if length > 0:
            return json.loads(self.rfile.read(length))
        return {}

    def do_GET(self):
        path = urlparse(self.path).path

        if path == '/status':
            self._send_json({
                'status': 'ok',
                'uptime': time.time(),
                'agents': list(allowances.keys()),
                'providers': list(vault.keys()),
                'budgets': {
                    name: {
                        'limit': a['daily_limit_usd'],
                        'used': round(a['used_today_usd'], 4),
                        'calls': a['calls'],
                        'reset_at': a['reset_at']
                    }
                    for name, a in allowances.items()
                }
            })

        elif path.startswith('/budget/'):
            agent = path.split('/')[-1]
            if agent in allowances:
                a = allowances[agent]
                self._send_json({
                    'agent': agent,
                    'daily_limit_usd': a['daily_limit_usd'],
                    'used_today_usd': round(a['used_today_usd'], 4),
                    'calls': a['calls'],
                    'remaining_usd': round(a['daily_limit_usd'] - a['used_today_usd'], 4),
                    'reset_at': a['reset_at']
                })
            else:
                self._send_json({'error': 'unknown agent'}, 404)

        else:
            self._send_json({'error': 'not found'}, 404)

    def do_POST(self):
        path = urlparse(self.path).path
        body = self._read_body()

        if path == '/auth':
            agent_name = body.get('agent')
            auth_token = body.get('token')

            if not agent_name or not auth_token:
                self._send_json({'error': 'agent and token required'}, 400)
                return

            if not verify_agent_token(auth_token, agent_name):
                self._send_json({'error': 'invalid token'}, 403)
                return

            # Create session
            session = hashlib.sha256(os.urandom(16)).hexdigest()[:24]
            sessions[session] = {
                'agent': agent_name,
                'expires': time.time() + 3600  # 1 hour session
            }

            self._send_json({
                'session': session,
                'expires_in': 3600,
                'agent': agent_name,
                'providers': list(vault.keys())
            })

        elif path.startswith('/key/'):
            provider = path.split('/')[-1]
            session_token = self.headers.get('X-Keeper-Session', '')
            estimated_cost = body.get('estimated_cost_usd', 0.001)

            # Verify session
            if session_token not in sessions:
                self._send_json({'error': 'not authenticated — POST /auth first'}, 401)
                return

            session = sessions[session_token]
            if time.time() > session['expires']:
                del sessions[session_token]
                self._send_json({'error': 'session expired'}, 401)
                return

            agent_name = session['agent']

            # Check budget
            ok, msg = check_budget(agent_name, estimated_cost)
            if not ok:
                self._send_json({'error': msg}, 429)
                return

            # Get key from vault
            if provider not in vault or not vault[provider].get('key'):
                self._send_json({'error': f'no key for provider: {provider}'}, 404)
                return

            # Record usage
            record_usage(agent_name, estimated_cost, provider)

            # Return key (masked for logs — never log the actual key)
            key = vault[provider]['key']
            self._send_json({
                'provider': provider,
                'key': key,
                'cost_usd': estimated_cost,
                'remaining_usd': round(
                    allowances[agent_name]['daily_limit_usd'] -
                    allowances[agent_name]['used_today_usd'], 4
                ),
                'note': 'key is ephemeral — do not store, re-request each session'
            })

        elif path.startswith('/register/'):
            # Register a new agent with budget
            agent_name = body.get('agent')
            daily_limit = body.get('daily_limit_usd', 1.0)

            if not agent_name:
                self._send_json({'error': 'agent name required'}, 400)
                return

            # Generate auth token for this agent
            token = make_agent_token(agent_name)

            allowances[agent_name] = {
                'daily_limit_usd': daily_limit,
                'used_today_usd': 0.0,
                'calls': 0,
                'reset_at': time.time() + 86400
            }
            save_allowances(DEFAULT_ALLOWANCES_PATH)

            self._send_json({
                'agent': agent_name,
                'token': token,
                'token_rotation': 'hourly',
                'daily_limit_usd': daily_limit,
                'usage': f'POST /key/{{provider}} with X-Keeper-Session header'
            })

        else:
            self._send_json({'error': 'not found'}, 404)


# ═══ Main ═══

def main():
    parser = argparse.ArgumentParser(description='Brothers Keeper Key Server')
    parser.add_argument('--port', type=int, default=DEFAULT_PORT)
    parser.add_argument('--vault', default=DEFAULT_VAULT_PATH)
    parser.add_argument('--secret', default=DEFAULT_SHARED_SECRET_PATH)
    args = parser.parse_args()

    load_vault(args.vault)
    load_allowances(DEFAULT_ALLOWANCES_PATH)
    load_secret(args.secret)

    print(f"Keeper Key Server starting on 127.0.0.1:{args.port}")
    print(f"Vault: {len(vault)} providers loaded")
    print(f"Allowances: {len(allowances)} agents registered")

    if not vault:
        print("WARNING: Vault is empty! Add keys to vault.json")

    server = HTTPServer(('127.0.0.1', args.port), KeeperHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down")
        save_allowances(DEFAULT_ALLOWANCES_PATH)
        server.server_close()


if __name__ == '__main__':
    main()
