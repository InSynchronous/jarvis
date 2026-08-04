#!/usr/bin/env python3
"""OAuth 2.0 login/refresh helper for the Google Workspace MCP servers.

Reads the server entry from mcp-servers.json (url + tokenFile) and the OAuth
client credentials from .env (GOOGLE_CLIENT_ID / GOOGLE_CLIENT_SECRET), then
performs the authorization-code flow with a loopback redirect URI.

Usage:
  python3 mcp/google_auth.py login  <server>   # full browser OAuth flow
  python3 mcp/google_auth.py refresh <server>  # refresh an existing token
  python3 mcp/google_auth.py status <server>   # show token state
  python3 mcp/google_auth.py logout <server>   # delete the stored token

Example:
  python3 mcp/google_auth.py login gmail
"""

import argparse
import json
import secrets
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import webbrowser
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

AUTH_URL = "https://accounts.google.com/o/oauth2/v2/auth"
TOKEN_URL = "https://oauth2.googleapis.com/token"
CALLBACK_PORT = 8342
REDIRECT_URI = f"http://127.0.0.1:{CALLBACK_PORT}/oauth/callback"

CONFIG_PATH = "mcp-servers.json"
ENV_PATH = ".env"

# Read-only scopes per Google Workspace MCP server (Developer Preview).
SCOPES = {
    "gmail": [
        "https://www.googleapis.com/auth/gmail.readonly",
        "https://www.googleapis.com/auth/gmail.compose",
    ],
    "drive": [
        "https://www.googleapis.com/auth/drive.readonly",
        "https://www.googleapis.com/auth/drive.file",
    ],
    "calendar": [
        "https://www.googleapis.com/auth/calendar.calendarlist.readonly",
        "https://www.googleapis.com/auth/calendar.events.freebusy",
        "https://www.googleapis.com/auth/calendar.events.readonly",
    ],
}


def die(msg, code=1):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(code)


def load_env():
    env = {}
    try:
        lines = Path(ENV_PATH).read_text().splitlines()
    except FileNotFoundError:
        return env
    for line in lines:
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, val = line.split("=", 1)
        key = key.strip()
        val = val.strip()
        if len(val) >= 2 and val[0] == val[-1] and val[0] in "\"'":
            val = val[1:-1]
        env[key] = val
    return env


def load_server_config(server):
    try:
        cfg = json.loads(Path(CONFIG_PATH).read_text())
    except FileNotFoundError:
        die(f"{CONFIG_PATH} not found; run from the project root")
    except json.JSONDecodeError as e:
        die(f"failed to parse {CONFIG_PATH}: {e}")
    srv = cfg.get("servers", {}).get(server)
    if not srv:
        known = ", ".join(cfg.get("servers", {}))
        die(f"no server named '{server}' in {CONFIG_PATH} (known: {known})")
    if not srv.get("tokenFile"):
        die(f"server '{server}' has no 'tokenFile' entry in {CONFIG_PATH}")
    return srv


def get_credentials():
    env = load_env()
    client_id = env.get("GOOGLE_CLIENT_ID")
    client_secret = env.get("GOOGLE_CLIENT_SECRET")
    if not client_id or not client_secret:
        die(
            "GOOGLE_CLIENT_ID / GOOGLE_CLIENT_SECRET not set in .env. "
            "Create a Web application OAuth client in the Google Cloud console "
            "and register redirect URI " + REDIRECT_URI
        )
    return client_id, client_secret


def write_tokens(path, tok):
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(tok, indent=2))
    print(f"Wrote tokens to {path}")


def read_tokens(path):
    target = Path(path)
    if not target.exists():
        die(f"no token file at {path}; run 'python3 mcp/google_auth.py login {args.server}'")
    try:
        return json.loads(target.read_text())
    except json.JSONDecodeError as e:
        die(f"failed to parse token file {path}: {e}")


def post_token(fields):
    data = urllib.parse.urlencode(fields).encode()
    req = urllib.request.Request(TOKEN_URL, data=data)
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return json.loads(resp.read().decode()), resp.status
    except urllib.error.HTTPError as e:
        body = e.read().decode(errors="replace")
        die(f"token endpoint returned HTTP {e.code}: {body}")


class CallbackHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        params = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
        code = params.get("code", [None])[0]
        state = params.get("state", [None])[0]
        self.server.code = code
        self.server.state = state
        if code:
            body = b"<html><body><h2>Authorization complete.</h2><p>You can close this tab.</p></body></html>"
            self.send_response(200)
        else:
            body = b"<html><body><h2>Authorization failed.</h2><p>No code received.</p></body></html>"
            self.send_response(400)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        pass


def cmd_login(server, scopes):
    srv = load_server_config(server)
    client_id, client_secret = get_credentials()
    state = secrets.token_urlsafe(16)

    auth_params = {
        "response_type": "code",
        "client_id": client_id,
        "redirect_uri": REDIRECT_URI,
        "scope": " ".join(scopes),
        "state": state,
        "access_type": "offline",
        "prompt": "consent",
    }
    auth_url = AUTH_URL + "?" + urllib.parse.urlencode(auth_params)

    httpd = HTTPServer(("127.0.0.1", CALLBACK_PORT), CallbackHandler)
    httpd.code = None
    httpd.state = None

    print(f"Open this URL in your browser and sign in:\n\n  {auth_url}\n")
    print(f"Waiting for the redirect to {REDIRECT_URI} ...")
    try:
        webbrowser.open(auth_url)
    except Exception:
        pass

    try:
        for _ in range(10):
            httpd.handle_request()
            if httpd.code:
                break
    finally:
        httpd.server_close()

    if not httpd.code:
        die("no authorization code received (timed out or flow cancelled)")
    if httpd.state != state:
        die("state mismatch; aborting (possible CSRF)")

    resp, _ = post_token(
        {
            "grant_type": "authorization_code",
            "code": httpd.code,
            "client_id": client_id,
            "client_secret": client_secret,
            "redirect_uri": REDIRECT_URI,
        }
    )
    if "access_token" not in resp:
        die("token response missing access_token: " + json.dumps(resp))

    resp["expires_at"] = int(time.time()) + resp.get("expires_in", 3600)
    write_tokens(srv["tokenFile"], resp)
    print(f"Login complete for '{server}'. Run jarvis to start using it.")


def cmd_refresh(server):
    srv = load_server_config(server)
    client_id, client_secret = get_credentials()
    tok = read_tokens(srv["tokenFile"])

    if not tok.get("refresh_token"):
        die(f"no refresh_token in {srv['tokenFile']}; run 'login' again")

    resp, _ = post_token(
        {
            "grant_type": "refresh_token",
            "refresh_token": tok["refresh_token"],
            "client_id": client_id,
            "client_secret": client_secret,
        }
    )
    if "access_token" not in resp:
        die("refresh failed, token response: " + json.dumps(resp))

    tok["access_token"] = resp["access_token"]
    tok["token_type"] = resp.get("token_type", "Bearer")
    tok["expires_at"] = int(time.time()) + resp.get("expires_in", 3600)
    if resp.get("refresh_token"):
        tok["refresh_token"] = resp["refresh_token"]
    write_tokens(srv["tokenFile"], tok)
    print(f"Refreshed token for '{server}'.")


def cmd_status(server):
    srv = load_server_config(server)
    target = Path(srv["tokenFile"])
    if not target.exists():
        print(f"{server}: no token file at {srv['tokenFile']} (not logged in)")
        return
    tok = json.loads(target.read_text())
    expires = tok.get("expires_at", 0)
    now = int(time.time())
    if expires and expires > now:
        print(f"{server}: valid, expires in {expires - now} seconds")
    elif expires:
        print(f"{server}: access token expired; run 'python3 mcp/google_auth.py refresh {server}'")
    else:
        print(f"{server}: token file present, no expiry info")
    if tok.get("refresh_token"):
        print(f"{server}: refresh_token available")


def cmd_logout(server):
    srv = load_server_config(server)
    target = Path(srv["tokenFile"])
    if target.exists():
        target.unlink()
        print(f"Deleted {srv['tokenFile']}")
    else:
        print(f"No token file at {srv['tokenFile']}")


def main():
    parser = argparse.ArgumentParser(description="Google Workspace MCP OAuth helper")
    parser.add_argument("command", choices=["login", "refresh", "status", "logout"])
    parser.add_argument("server")
    parser.add_argument("--scopes", nargs="*", help="override the default scopes for this server")
    args = parser.parse_args()

    if args.command == "login":
        scopes = args.scopes or SCOPES.get(args.server)
        if not scopes:
            die(f"no default scopes for '{args.server}'; pass --scopes explicitly")
        cmd_login(args.server, scopes)
    elif args.command == "refresh":
        cmd_refresh(args.server)
    elif args.command == "status":
        cmd_status(args.server)
    elif args.command == "logout":
        cmd_logout(args.server)


if __name__ == "__main__":
    main()
