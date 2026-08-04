#!/usr/bin/env python3
"""Local Gmail MCP server backed by the Gmail REST API.

Bypasses the Google Workspace MCP gateway (Developer Preview gating) and talks
directly to gmail.googleapis.com using the OAuth token managed by
mcp/google_auth.py. Reads the token from GMAIL_TOKEN_FILE (default
mcp/tokens/gmail.json) and lazily refreshes it before each call.

Scopes required (see mcp/google_auth.py): gmail.readonly + gmail.compose.
Sending mail needs the additional gmail.send scope.
"""

import base64
import json
import os
import time
import urllib.error
import urllib.parse
import urllib.request
from email.message import EmailMessage
from pathlib import Path

from mcp.server.fastmcp import FastMCP

mcp = FastMCP("Gmail REST")

API_BASE = "https://gmail.googleapis.com/gmail/v1/users/me"
TOKEN_URL = "https://oauth2.googleapis.com/token"
TOKEN_FILE = os.environ.get("GMAIL_TOKEN_FILE", "mcp/tokens/gmail.json")
ENV_PATH = ".env"

_CREDENTIALS = None


def _load_env():
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


def _credentials():
    global _CREDENTIALS
    if _CREDENTIALS is None:
        env = _load_env()
        _CREDENTIALS = (env.get("GOOGLE_CLIENT_ID"), env.get("GOOGLE_CLIENT_SECRET"))
    return _CREDENTIALS


def _read_token_file():
    path = Path(TOKEN_FILE)
    if not path.exists():
        raise RuntimeError(
            f"No token file at {TOKEN_FILE}. Run 'python3 mcp/google_auth.py login gmail'."
        )
    return json.loads(path.read_text())


def _write_token_file(tok):
    path = Path(TOKEN_FILE)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(tok, indent=2))


def _refresh():
    tok = _read_token_file()
    refresh_token = tok.get("refresh_token")
    client_id, client_secret = _credentials()
    if not refresh_token:
        raise RuntimeError(
            f"No refresh_token in {TOKEN_FILE}. Run 'python3 mcp/google_auth.py login gmail'."
        )
    if not client_id or not client_secret:
        raise RuntimeError(
            "GOOGLE_CLIENT_ID / GOOGLE_CLIENT_SECRET not set in .env; "
            "cannot refresh an expired token."
        )
    data = urllib.parse.urlencode(
        {
            "grant_type": "refresh_token",
            "refresh_token": refresh_token,
            "client_id": client_id,
            "client_secret": client_secret,
        }
    ).encode()
    req = urllib.request.Request(TOKEN_URL, data=data)
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            new = json.loads(resp.read().decode())
    except urllib.error.HTTPError as e:
        body = e.read().decode(errors="replace")
        raise RuntimeError(f"Token refresh failed (HTTP {e.code}): {body}") from e
    if "access_token" not in new:
        raise RuntimeError("Token refresh failed: " + json.dumps(new))
    tok["access_token"] = new["access_token"]
    tok["token_type"] = new.get("token_type", "Bearer")
    tok["expires_at"] = int(time.time()) + new.get("expires_in", 3600)
    if new.get("refresh_token"):
        tok["refresh_token"] = new["refresh_token"]
    _write_token_file(tok)
    return tok["access_token"]


def _access_token():
    tok = _read_token_file()
    expires = tok.get("expires_at", 0)
    if expires and expires - int(time.time()) > 60:
        return tok["access_token"]
    return _refresh()


def _api(path, method="GET", body=None):
    url = API_BASE + path
    if body is not None:
        data = json.dumps(body).encode()
    else:
        data = None
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Authorization", "Bearer " + _access_token())
    req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            raw = resp.read()
            return json.loads(raw.decode()) if raw else {}
    except urllib.error.HTTPError as e:
        detail = e.read().decode(errors="replace")
        if e.code == 401:
            raise RuntimeError(
                f"Gmail API returned 401 (token rejected). "
                f"Run 'python3 mcp/google_auth.py refresh gmail' and retry."
            ) from e
        raise RuntimeError(f"Gmail API error {e.code}: {detail}") from e


def _extract_headers(payload):
    headers = {}
    for h in payload.get("headers", []):
        name = h.get("name", "")
        if name in ("Subject", "From", "To", "Cc", "Bcc", "Date"):
            headers[name.lower()] = h.get("value", "")
    return headers


def _collect_text(payload, texts):
    mime = payload.get("mimeType", "")
    data = payload.get("body", {}).get("data")
    if data and mime in ("text/plain", "text/html"):
        try:
            texts[mime] = base64.urlsafe_b64decode(data).decode("utf-8", errors="replace")
        except Exception:
            pass
    for part in payload.get("parts", []):
        _collect_text(part, texts)


def _format_message(msg, message_format):
    msg_id = msg.get("id")
    headers = _extract_headers(msg.get("payload", {}))
    fmt = (message_format or "FULL_CONTENT").upper()
    if fmt in ("MINIMAL", "METADATA_ONLY") or fmt in ("MINIMAL", "METADATA"):
        return {
            "id": msg_id,
            "threadId": msg.get("threadId"),
            "snippet": msg.get("snippet", ""),
            "labelIds": msg.get("labelIds", []),
            "sender": headers.get("from", ""),
            "subject": headers.get("subject", ""),
            "date": headers.get("date", ""),
            "toRecipients": _split_addresses(headers.get("to", "")),
            "ccRecipients": _split_addresses(headers.get("cc", "")),
            "bccRecipients": _split_addresses(headers.get("bcc", "")),
            "internalDate": msg.get("internalDate"),
        }
    texts = {}
    _collect_text(msg.get("payload", {}), texts)
    return {
        "id": msg_id,
        "threadId": msg.get("threadId"),
        "snippet": msg.get("snippet", ""),
        "labelIds": msg.get("labelIds", []),
        "sender": headers.get("from", ""),
        "subject": headers.get("subject", ""),
        "date": headers.get("date", ""),
        "toRecipients": _split_addresses(headers.get("to", "")),
        "ccRecipients": _split_addresses(headers.get("cc", "")),
        "bccRecipients": _split_addresses(headers.get("bcc", "")),
        "plaintextBody": texts.get("text/plain", ""),
        "htmlBody": texts.get("text/html", ""),
    }


def _split_addresses(value):
    if not value:
        return []
    return [a.strip() for a in value.split(",") if a.strip()]


def _build_mime(to, subject, body, cc=None, bcc=None, reply_to_thread=None):
    msg = EmailMessage()
    if to:
        msg["To"] = ", ".join(to)
    if cc:
        msg["Cc"] = ", ".join(cc)
    if bcc:
        msg["Bcc"] = ", ".join(bcc)
    msg["Subject"] = subject or ""
    msg.set_content(body or "")
    raw = msg.as_bytes()
    encoded = base64.urlsafe_b64encode(raw).decode()
    draft_message = {"raw": encoded}
    if reply_to_thread:
        draft_message["threadId"] = reply_to_thread
    return draft_message


@mcp.tool()
def get_profile() -> str:
    """Return the authenticated Gmail account's email address and unread message count.

    Use this as a quick connectivity/identity check for the gmail account.
    """
    prof = _api("/profile")
    return json.dumps(prof, indent=2)


@mcp.tool()
def search_threads(
    query: str = "",
    pageSize: int = 20,
    includeTrash: bool = False,
    view: str = "THREAD_VIEW_MINIMAL",
) -> str:
    """Search the authenticated user's Gmail for email threads.

    Args:
        query: Gmail search syntax (e.g. "from:alice@example.com newer_than:7d",
            "is:unread", "subject:hello"). Empty returns all threads.
        pageSize: Max threads to return (default 20, max 50).
        includeTrash: Include threads from TRASH (default false).
        view: THREAD_VIEW_MINIMAL returns subject/snippet/sender/date/labels;
            THREAD_VIEW_METADATA_ONLY omits subject and snippet.
    """
    pageSize = min(max(pageSize, 1), 50)
    params = {"maxResults": pageSize}
    if query:
        params["q"] = query
    if includeTrash:
        params["includeSpamTrash"] = "true"
    path = "/threads?" + urllib.parse.urlencode(params)
    data = _api(path)
    threads = []
    for t in data.get("threads", []):
        detail = _api(f"/threads/{t['id']}?format=metadata")
        messages = detail.get("messages") or []
        if not messages:
            continue
        first = _format_message(messages[0], "METADATA")
        first["id"] = t["id"]
        first["snippet"] = t.get("snippet", "")
        if view == "THREAD_VIEW_METADATA_ONLY":
            first.pop("subject", None)
            first.pop("snippet", None)
        first["messages"] = [m["id"] for m in messages]
        threads.append(first)
    return json.dumps({"resultCountEstimate": len(threads), "threads": threads}, indent=2)


@mcp.tool()
def get_thread(threadId: str, messageFormat: str = "FULL_CONTENT") -> str:
    """Retrieve a full email thread by its thread ID, including all messages.

    Args:
        threadId: The thread ID (from search_threads).
        messageFormat: FULL_CONTENT (default, includes bodies), MINIMAL
            (subject/snippet only), or METADATA_ONLY (headers only).
    """
    detail = _api(f"/threads/{urllib.parse.quote(threadId)}?format=full")
    messages = [_format_message(m, messageFormat) for m in detail.get("messages", [])]
    return json.dumps({"id": detail.get("id"), "messages": messages}, indent=2)


@mcp.tool()
def get_message(messageId: str, messageFormat: str = "FULL_CONTENT") -> str:
    """Retrieve a single email message by its message ID.

    Args:
        messageId: The message ID (from search_threads or get_thread).
        messageFormat: FULL_CONTENT (default, includes bodies), MINIMAL,
            or METADATA_ONLY.
    """
    msg = _api(f"/messages/{urllib.parse.quote(messageId)}?format=full")
    return json.dumps(_format_message(msg, messageFormat), indent=2)


@mcp.tool()
def list_labels() -> str:
    """List all labels in the user's Gmail account with message/thread counts."""
    data = _api("/labels")
    return json.dumps(data.get("labels", []), indent=2)


@mcp.tool()
def create_draft(
    to: list[str],
    subject: str,
    body: str,
    cc: list[str] = None,
    bcc: list[str] = None,
    replyToMessageId: str = "",
) -> str:
    """Create a new draft email in the user's Gmail account. Does NOT send it.

    Args:
        to: List of recipient email addresses.
        subject: Subject line.
        body: Plain-text body content.
        cc: Optional list of CC recipients.
        bcc: Optional list of BCC recipients.
        replyToMessageId: If set, create the draft as a reply to this message.
    """
    msg = _build_mime(to, subject, body, cc or [], bcc or [], replyToMessageId)
    result = _api("/drafts", method="POST", body={"message": msg})
    return json.dumps({"id": result.get("id"), "threadId": result.get("message", {}).get("threadId")}, indent=2)


if __name__ == "__main__":
    mcp.run()
