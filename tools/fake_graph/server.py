#!/usr/bin/env python3
"""Fake Microsoft identity platform + Microsoft Graph v1.0 server for Slow Mail.

Python 3 standard library only.  Emulates the subset Slow Mail uses (device-code OAuth,
inbox delta, message get/move/patch, attachment metadata) on ONE port, plus a JSON
control API under /__control/ for fixtures and fault injection.  See README.md.

    python3 tools/fake_graph/server.py --port 8765 --generate 50 --auto-approve-after 2
    python3 tools/fake_graph/server.py --port 0        # prints "LISTENING <port>" first
"""
from __future__ import annotations

import argparse
import base64
import bisect
import collections
import dataclasses
import hashlib
import html
import json
import os
import random
import re
import signal
import socket
import sys
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Deque, Dict, Iterable, List, Optional, Tuple
from urllib.parse import parse_qs, unquote, urlsplit

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:  # python3 -I does not add the script directory
    sys.path.insert(0, HERE)
import generate_fixture  # noqa: E402

FOLDERS = ("inbox", "archive", "deleteditems")
DISPLAY = {"inbox": "Inbox", "archive": "Archive", "deleteditems": "Deleted Items"}
ROOT_FOLDER_ID = "AAMkAGROOT0msgfolderrootAAA="
USER_ID = "00000000-0000-4000-8000-0000000fa4e1"
USER_UPN = "slowmail.test@example.test"
TENANT_GUID = "11111111-2222-4333-8444-555555555555"
FLAG_VALUES = ("notFlagged", "flagged", "complete")
READ_ONLY_FIELDS = {"subject", "body", "from", "sender", "toRecipients", "ccRecipients", "bccRecipients",
                    "bodyPreview", "receivedDateTime", "sentDateTime", "conversationId", "internetMessageId"}
ISO_RE = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d+)?Z$")


def now_iso() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def folder_id(name: str) -> str:
    return "AAMkAGFLD" + base64.b64encode(("folder:" + name).encode()).decode().rstrip("=") + "AAA="


FOLDER_IDS = {n: folder_id(n) for n in FOLDERS}


# --------------------------------------------------------------------------- config
@dataclasses.dataclass
class Config:
    token_ttl: float = 3600.0
    delta_expiry_changes: int = 100000
    delta_expiry_status: int = 410        # 410 (default) or 400 for an expired/unknown delta token
    delta_partial_updates: bool = True    # updated messages: id + changed fields + changeKey only
    delta_replay: bool = False            # repeat last item of each page at the start of the next
    immutable_gap: bool = False           # known Graph issue: moved out and back in => only @removed
    trailing_empty_page: bool = False     # emit an empty final page carrying the deltaLink
    device_interval: float = 5.0
    device_expires_in: int = 900
    auto_approve_after: int = 0
    strict_interval: bool = True          # slow_down when polled faster than interval
    slow_down_increment: float = 5.0
    strict_refresh: bool = True           # refresh rotates tokens; the old refresh token dies
    has_archive: bool = True              # False: archive well-known folder missing
    doc_style_odata_type: bool = False    # attachment @odata.type without leading '#'
    default_page_size: int = 10
    max_page_size: int = 1000

    def update(self, values: Dict[str, Any]) -> None:
        fields = {f.name: f for f in dataclasses.fields(self)}
        for key, value in values.items():
            if key not in fields:
                raise ValueError(f"unknown config key: {key}")
            current = getattr(self, key)
            if isinstance(current, bool):
                value = bool(value)
            elif isinstance(current, int):
                value = int(value)
            elif isinstance(current, float):
                value = float(value)
            setattr(self, key, value)


# --------------------------------------------------------------------------- helpers
def html_to_text(src: str) -> str:
    """Crude HTML -> text: drop script/style/head, tags and comments; \\r\\n line endings."""
    s = re.sub(r"(?is)<(script|style|head)\b.*?</\1\s*>", "", src)
    s = re.sub(r"(?s)<!--.*?-->", "", s)
    s = re.sub(r"(?i)</?(?:p|div|br|tr|li|h[1-6]|blockquote|table|ul|ol|hr)\b[^>]*>", "\n", s)
    s = re.sub(r"<[^>]*>", "", s)
    s = html.unescape(s).replace("\xa0", " ").replace("\r", "")
    out: List[str] = []
    blank = False
    for line in (ln.strip() for ln in s.split("\n")):
        if not line:
            if not blank and out:
                out.append("")
            blank = True
        else:
            out.append(line)
            blank = False
    return "\r\n".join(out).strip()


def text_to_html(text: str) -> str:
    return "<html><head></head><body>" + html.escape(text).replace("\n", "<br>") + "</body></html>"


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).decode().rstrip("=")


def encode_state(state: Dict[str, Any]) -> str:
    return b64url(json.dumps(state, separators=(",", ":"), sort_keys=True).encode())


def decode_state(token: str) -> Optional[Dict[str, Any]]:
    try:
        raw = base64.urlsafe_b64decode(token + "=" * (-len(token) % 4))
        obj = json.loads(raw.decode())
        return obj if isinstance(obj, dict) else None
    except Exception:  # noqa: BLE001 - anything malformed means "unknown token"
        return None


def parse_prefer(headers: Any) -> Dict[str, str]:
    prefs: Dict[str, str] = {}
    for value in headers.get_all("Prefer") or []:
        for name, val in re.findall(r'([^,=\s]+)\s*(?:=\s*("[^"]*"|[^,]*))?', value):
            prefs[name.lower()] = val.strip().strip('"')
    return prefs


class ApiError(Exception):
    def __init__(self, status: int, code: str, message: str = "", headers: Optional[List[Tuple[str, str]]] = None):
        super().__init__(message or code)
        self.status, self.code, self.message, self.headers = status, code, message or code, headers or []


class Resp:
    def __init__(self, status: int = 200, body: Any = None, headers: Optional[List[Tuple[str, str]]] = None):
        self.status = status
        self.body = body
        self.headers = headers or []


def error_body(code: str, message: str, request_id: str = "", client_id: str = "") -> Dict[str, Any]:
    return {"error": {"code": code, "message": message,
                      "innerError": {"date": now_iso(), "request-id": request_id or str(uuid.uuid4()),
                                     "client-request-id": client_id or request_id or str(uuid.uuid4())}}}


# --------------------------------------------------------------------------- mailbox model
class Message:
    """One mailbox item. `key` is the immutable id; `mut` the mutable id (changes on every move)."""

    def __init__(self, key: str, folder: str, props: Dict[str, Any], body: Dict[str, str],
                 attachments: List[Dict[str, Any]], flag: Dict[str, Any], is_read: bool,
                 has_attachments: bool, preview: str, seq: int):
        self.key = key
        self.folder = folder
        self.props = props            # passthrough Graph properties (subject, from, receivedDateTime, ...)
        self.body = body
        self.attachments = attachments
        self.flag = flag
        self.is_read = is_read
        self.has_attachments = has_attachments
        self.preview = preview
        self.seq = seq
        self.version = 0
        self.mut = self._mut_id()
        self.change_n = 0
        self.change_key = self._change_key()
        self.modified = now_iso()

    def _mut_id(self) -> str:
        return "AAMkMUT" + base64.b32encode(hashlib.sha1(f"{self.key}:{self.version}".encode()).digest()).decode().rstrip("=")[:30] + "="

    def _change_key(self) -> str:
        digest = hashlib.sha1(f"{self.key}:{self.change_n}".encode()).digest()
        return "CQAAABYAAAB" + base64.b64encode(digest).decode().rstrip("=").replace("+", "-").replace("/", "_")[:24]

    def bump(self) -> None:
        self.change_n += 1
        self.change_key = self._change_key()
        self.modified = now_iso()

    @property
    def received(self) -> str:
        return str(self.props.get("receivedDateTime", ""))

    def render_body(self, pref: str) -> Dict[str, str]:
        ctype, content = self.body["contentType"], self.body["content"]
        if pref == "text":
            return {"contentType": "text", "content": html_to_text(content) if ctype == "html" else content}
        return {"contentType": "html", "content": content if ctype == "html" else text_to_html(content)}

    def render(self, ctx: "Ctx", fields: Optional[List[str]] = None) -> Dict[str, Any]:
        full: Dict[str, Any] = {"@odata.type": "#microsoft.graph.message", "@odata.etag": f'W/"{self.change_key}"',
                                "id": self.key if ctx.immutable else self.mut}
        full.update(self.props)
        full.update({"changeKey": self.change_key, "lastModifiedDateTime": self.modified,
                     "hasAttachments": self.has_attachments, "bodyPreview": self.preview,
                     "parentFolderId": FOLDER_IDS[self.folder], "isRead": self.is_read, "flag": self.flag})
        wants_body = fields is None or any(f.lower() in ("body", "uniquebody") for f in fields)
        if wants_body:
            full["body"] = self.render_body(ctx.body_pref)
            if fields is not None and any(f.strip().lower() == "uniquebody" for f in fields):
                full["uniqueBody"] = full["body"]
        if fields is None:
            return full
        out = {k: full[k] for k in ("@odata.type", "@odata.etag", "id")}
        lower = {k.lower(): k for k in full}
        for f in fields:
            k = lower.get(f.strip().lower())
            if k and k not in out:
                out[k] = full[k]
        return out

    def render_partial(self, ctx: "Ctx", changed: Iterable[str]) -> Dict[str, Any]:
        out: Dict[str, Any] = {"@odata.type": "#microsoft.graph.message", "@odata.etag": f'W/"{self.change_key}"',
                               "id": self.key if ctx.immutable else self.mut, "changeKey": self.change_key}
        for name in sorted(changed):
            if name == "flag":
                out["flag"] = self.flag
            elif name == "isRead":
                out["isRead"] = self.is_read
        return out

    def render_attachments(self, select: Optional[List[str]], doc_style: bool) -> Tuple[List[Dict[str, Any]], int]:
        """Attachment metadata. contentBytes is served only when $select is absent or names it."""
        served = 0
        result: List[Dict[str, Any]] = []
        for att in self.attachments:
            item = dict(att)
            is_file = "fileattachment" in str(att.get("@odata.type", "")).lower()
            if doc_style:
                item["@odata.type"] = str(item.get("@odata.type", "")).lstrip("#")
            if select is not None:
                keep = {s.strip().lower() for s in select}
                item = {k: v for k, v in item.items() if k == "@odata.type" or k.lower() in keep}
            if not is_file or select is not None and "contentbytes" not in {s.strip().lower() for s in select}:
                item.pop("contentBytes", None)
            elif "contentBytes" in item:
                served += 1
            result.append(item)
        return result, served


@dataclasses.dataclass
class Ctx:
    """Per-request rendering context derived from the Prefer header."""
    immutable: bool
    body_pref: str
    base: str


Entry = Tuple[str, str, str, Optional[frozenset]]  # kind a|u|r, key, mutable id before, changed fields


class Mailbox:
    def __init__(self) -> None:
        self.generation = 0
        self.clear()

    def clear(self) -> None:
        self.generation += 1
        self.messages: Dict[str, Message] = {}
        self.mut_index: Dict[str, str] = {}
        self.members: Dict[str, Dict[str, Message]] = {n: {} for n in FOLDERS}
        self.logs: Dict[str, List[Entry]] = {n: [] for n in FOLDERS}
        self._sorted: Dict[str, Optional[List[Tuple[str, str]]]] = {n: None for n in FOLDERS}
        self.seq = 0
        self.auto_id = 0

    # -- queries
    def sorted_keys(self, folder: str) -> List[Tuple[str, str]]:
        cached = self._sorted[folder]
        if cached is None:
            cached = sorted((m.received, m.key) for m in self.members[folder].values())
            self._sorted[folder] = cached
        return cached

    def find(self, ident: str, immutable: bool) -> Optional[Message]:
        if immutable:
            return self.messages.get(ident)
        key = self.mut_index.get(ident)
        return self.messages.get(key) if key else None

    def find_any(self, ident: str) -> Optional[Message]:
        return self.messages.get(ident) or self.find(ident, False)

    # -- mutations (each appends to the folder change log)
    def add(self, m: Message, folder: str) -> None:
        m.folder = folder
        self.messages[m.key] = m
        self.mut_index[m.mut] = m.key
        self.members[folder][m.key] = m
        self.logs[folder].append(("a", m.key, m.mut, None))
        self._sorted[folder] = None

    def move(self, m: Message, dest: str) -> None:
        src = m.folder
        if src == dest:
            return
        self.logs[src].append(("r", m.key, m.mut, None))
        del self.members[src][m.key]
        self._sorted[src] = None
        del self.mut_index[m.mut]
        m.version += 1
        m.mut = m._mut_id()
        self.mut_index[m.mut] = m.key
        m.folder = dest
        m.bump()
        self.members[dest][m.key] = m
        self._sorted[dest] = None
        self.logs[dest].append(("a", m.key, m.mut, None))

    def update(self, m: Message, fields: Iterable[str]) -> None:
        m.bump()
        self.logs[m.folder].append(("u", m.key, m.mut, frozenset(fields)))

    def hard_delete(self, m: Message) -> None:
        self.logs[m.folder].append(("r", m.key, m.mut, None))
        del self.members[m.folder][m.key]
        self._sorted[m.folder] = None
        del self.mut_index[m.mut]
        del self.messages[m.key]

    # -- delta change computation for an incremental round
    def changes(self, folder: str, start: int, end: int, ctx: Ctx, partial: bool,
                gap: bool) -> List[Tuple[str, Any, Any]]:
        """Returns ('removed', id, None) | ('full', Message, None) | ('partial', Message, fields)."""
        order: List[str] = []
        per_key: Dict[str, List[Entry]] = {}
        for entry in self.logs[folder][start:end]:
            if entry[1] not in per_key:
                per_key[entry[1]] = []
                order.append(entry[1])
            per_key[entry[1]].append(entry)
        out: List[Tuple[str, Any, Any]] = []
        for key in order:
            entries = per_key[key]
            first = entries[0]
            msg = self.messages.get(key)
            here = msg is not None and msg.folder == folder
            removed = [e for e in entries if e[0] == "r"]
            if not here:
                out.append(("removed", key if ctx.immutable else first[2], None))
                continue
            assert msg is not None
            if removed and ctx.immutable and gap:
                out.append(("removed", key, None))
                continue
            if removed and not ctx.immutable and first[2] != msg.mut:
                out.append(("removed", first[2], None))
            if any(e[0] == "a" for e in entries) or not partial:
                out.append(("full", msg, None))
            else:
                fields: set = set()
                for e in entries:
                    fields |= set(e[3] or ())
                out.append(("partial", msg, fields))
        return out


# --------------------------------------------------------------------------- application
class Req:
    def __init__(self, method: str, target: str, headers: Any, body: bytes, host: str):
        self.method, self.target, self.headers, self.body, self.host = method, target, headers, body, host
        parts = urlsplit(target)
        self.raw_path = parts.path
        self.path = unquote(parts.path)
        self.query = parse_qs(parts.query, keep_blank_values=True)
        self.base = f"http://{host}"
        self.prefs = parse_prefer(headers)
        self.request_id = str(uuid.uuid4())
        self.client_request_id = headers.get("client-request-id") or self.request_id

    def q(self, name: str) -> Optional[str]:
        vals = self.query.get(name)
        return vals[0] if vals else None

    def json(self) -> Any:
        if not self.body.strip():
            return {}
        try:
            return json.loads(self.body.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            raise ApiError(400, "ErrorInvalidRequest", "Request body is not valid JSON.")

    def form(self) -> Dict[str, str]:
        parsed = parse_qs(self.body.decode("utf-8", "replace"), keep_blank_values=True)
        return {k: v[0] for k, v in parsed.items()}


def normalise_path(method: str, path: str) -> str:
    segs = path.split("/")
    out: List[str] = []
    i = 0
    while i < len(segs):
        seg = segs[i]
        out.append(seg)
        if seg in ("messages", "attachments") and i + 1 < len(segs) and segs[i + 1] not in ("delta", ""):
            out.append("{id}")
            i += 1
        i += 1
    joined = "/".join(out)
    joined = re.sub(r"^/[^/]+/oauth2/v2\.0/", "/{tenant}/oauth2/v2.0/", joined)
    return f"{method} {joined}"


class App:
    def __init__(self, config: Config, verbose: bool = False):
        self.lock = threading.RLock()
        self.initial_config = dataclasses.replace(config)
        self.config = config
        self.verbose = verbose
        self.mailbox = Mailbox()
        self.reset_runtime(keep_auth=False)

    # ---- state management
    def reset_runtime(self, keep_auth: bool) -> None:
        if not keep_auth or not hasattr(self, "devices"):
            self.devices: Dict[str, Dict[str, Any]] = {}
            self.access_tokens: Dict[str, Dict[str, Any]] = {}
            self.refresh_tokens: Dict[str, Dict[str, Any]] = {}
        self.faults: List[Dict[str, Any]] = []
        self.drops: List[Dict[str, Any]] = []
        self.counters: Dict[str, Any] = {"requests": 0, "forbidden_fetch": 0, "content_bytes_served": 0,
                                         "delete_calls": 0, "graph_401": 0, "refreshes": 0,
                                         "device_codes_issued": 0, "by_path": {}}
        self.log: Deque[Dict[str, Any]] = collections.deque(maxlen=200)
        self.seq = 0

    def reset(self, keep_auth: bool = False) -> None:
        self.mailbox.clear()
        self.config = dataclasses.replace(self.initial_config)
        self.reset_runtime(keep_auth)

    # ---- fixture loading
    def load_data(self, data: Any, replace: bool = True) -> int:
        messages = data.get("messages") if isinstance(data, dict) else data
        if not isinstance(messages, list):
            raise ValueError("fixture must be a list of messages or an object with a 'messages' list")
        if replace:
            self.mailbox.clear()
        for raw in messages:
            self.add_message(raw)
        return len(messages)

    def load_file(self, path: str, replace: bool = True) -> int:
        with open(path, "r", encoding="utf-8") as fh:
            return self.load_data(json.load(fh), replace)

    def add_message(self, raw: Dict[str, Any], folder: Optional[str] = None) -> Message:
        if not isinstance(raw, dict):
            raise ValueError("message must be an object")
        mb = self.mailbox
        raw = dict(raw)
        dest = str(folder or raw.pop("folder", "inbox")).lower()
        raw.pop("folder", None)
        if dest not in FOLDERS:
            raise ValueError(f"unknown folder: {dest}")
        mb.auto_id += 1
        key = str(raw.pop("id", "") or f"AAMkAGAUTO{mb.auto_id:08d}AAA=")
        while key in mb.messages:
            key += "x"
        body = raw.pop("body", None) or {"contentType": "text", "content": ""}
        ctype = "html" if str(body.get("contentType", "text")).lower() == "html" else "text"
        body = {"contentType": ctype, "content": str(body.get("content", ""))}
        atts: List[Dict[str, Any]] = []
        for n, a in enumerate(raw.pop("attachments", None) or []):
            a = dict(a)
            a.setdefault("@odata.type", "#microsoft.graph.fileAttachment")
            a.setdefault("id", f"AAMkATT{mb.auto_id:06d}-{n}=")
            a.setdefault("name", "attachment")
            a.setdefault("isInline", False)
            if "fileattachment" in a["@odata.type"].lower():
                a.setdefault("contentBytes", "AAAA")
            atts.append(a)
        flag = raw.pop("flag", None) or {"flagStatus": "notFlagged"}
        if flag.get("flagStatus") not in FLAG_VALUES:
            raise ValueError("flag.flagStatus must be notFlagged|flagged|complete")
        is_read = bool(raw.pop("isRead", False))
        explicit_has = raw.pop("hasAttachments", None)
        has = bool(explicit_has) if explicit_has is not None else any(not a.get("isInline") for a in atts)
        text = html_to_text(body["content"]) if ctype == "html" else body["content"]
        preview = str(raw.pop("bodyPreview", None) if "bodyPreview" in raw else text[:255])
        raw.pop("changeKey", None)
        raw.pop("parentFolderId", None)
        received = raw.get("receivedDateTime") or now_iso()
        raw["receivedDateTime"] = received
        raw.setdefault("sentDateTime", received)
        raw.setdefault("createdDateTime", received)
        raw.setdefault("subject", "")
        raw.setdefault("conversationId", "AAQkCONV" + hashlib.sha1(key.encode()).hexdigest()[:20] + "=")
        raw.setdefault("importance", "normal")
        raw.setdefault("isDraft", False)
        raw.setdefault("categories", [])
        raw.setdefault("internetMessageId", f"<{hashlib.sha1(key.encode()).hexdigest()[:16]}@mail.example.test>")
        if "from" in raw and "sender" not in raw:
            raw["sender"] = raw["from"]
        mb.seq += 1
        msg = Message(key, dest, raw, body, atts, flag, is_read, has, preview, mb.seq)
        mb.add(msg, dest)
        return msg

    # ---- request entry point
    def handle(self, req: Req) -> Tuple[Optional[Resp], bool]:
        """Returns (response, drop_after). response None => drop the connection before replying."""
        if req.path.startswith("/__control/"):
            with self.lock:
                try:
                    return self.control(req), False
                except ApiError as e:
                    return Resp(e.status, {"error": e.message}), False
                except (ValueError, KeyError, TypeError, OSError) as e:
                    return Resp(400, {"error": f"{type(e).__name__}: {e}"}), False
        with self.lock:
            self.counters["requests"] += 1
            key = normalise_path(req.method, req.raw_path)
            by_path = self.counters["by_path"]
            by_path[key] = by_path.get(key, 0) + 1
            self.seq += 1
            entry = {"seq": self.seq, "method": req.method, "path": req.path, "query": urlsplit(req.target).query,
                     "headers": self._header_subset(req), "body_length": len(req.body), "status": None}
            self.log.append(entry)
            drop = self._take(self.drops, req)
            if drop is not None and not drop.get("after"):
                entry["status"] = 0
                entry["dropped"] = True
                return None, False
            fault = self._take(self.faults, req)
            delay = float(fault.get("delay", 0)) if fault else 0.0
            try:
                if fault is not None:
                    resp = self._fault_response(fault, req)
                else:
                    resp = self.route(req)
            except ApiError as e:
                resp = Resp(e.status, error_body(e.code, e.message, req.request_id, req.client_request_id), e.headers)
            except Exception as e:  # noqa: BLE001 - a fake-server bug must surface as a 500, not a hang
                resp = Resp(500, error_body("InternalServerError", f"{type(e).__name__}: {e}"))
            entry["status"] = resp.status
            if drop is not None:
                entry["dropped"] = "after"
        if delay:
            time.sleep(delay)
        return resp, drop is not None

    @staticmethod
    def _header_subset(req: Req) -> Dict[str, str]:
        subset: Dict[str, str] = {}
        prefer = req.headers.get_all("Prefer")
        if prefer:
            subset["Prefer"] = ", ".join(prefer)
        for name in ("Content-Type", "Accept", "User-Agent", "If-Match"):
            if req.headers.get(name):
                subset[name] = req.headers.get(name)
        auth = req.headers.get("Authorization")
        subset["Authorization"] = "Bearer <redacted>" if auth else "<none>"
        return subset

    @staticmethod
    def _take(rules: List[Dict[str, Any]], req: Req) -> Optional[Dict[str, Any]]:
        for rule in rules:
            if rule.get("method") and rule["method"].upper() != req.method:
                continue
            if rule["match"] not in req.target and rule["match"] not in req.path:
                continue
            if rule["count"] > 0:
                rule["count"] -= 1
            used = rule
            rules[:] = [r for r in rules if r["count"] != 0]
            return used
        return None

    @staticmethod
    def _fault_response(fault: Dict[str, Any], req: Req) -> Resp:
        status = int(fault.get("status", 500))
        headers: List[Tuple[str, str]] = []
        retry = fault.get("retry_after", 1 if status in (429, 503) else None)
        if retry is not None:
            headers.append(("Retry-After", str(retry)))
        for k, v in (fault.get("headers") or {}).items():
            headers.append((k, str(v)))
        body = fault.get("body")
        if body is None:
            codes = {429: "TooManyRequests", 503: "ServiceNotAvailable", 500: "ErrorInternalServerError",
                     401: "InvalidAuthenticationToken", 403: "ErrorAccessDenied", 404: "ErrorItemNotFound",
                     410: "SyncStateNotFound", 400: "SyncStateNotFound" if "delta" in req.path else "BadRequest",
                     409: "ErrorIrresolvableConflict", 412: "ErrorIrresolvableConflict"}
            body = error_body(codes.get(status, "InjectedFault"), "Injected fault.", req.request_id, req.client_request_id)
            if status == 429:
                body["error"]["innerError"].update({"code": "429", "status": "429", "message": "Please retry after"})
        return Resp(status, body, headers)

    # ---- routing
    def route(self, req: Req) -> Resp:
        path = req.path
        m = re.match(r"^/([^/]+)/oauth2/v2\.0/(devicecode|token)$", path)
        if m:
            return self.oauth(req, m.group(2))
        if not path.startswith("/v1.0/"):
            raise ApiError(404, "ResourceNotFound", f"Unknown resource {path}")
        # '$value' fetches are forbidden by the fake whether or not the caller is authenticated
        if path.endswith("/$value"):
            self.counters["forbidden_fetch"] += 1
            self._check_auth(req)
            raise ApiError(403, "ErrorAccessDenied", "Fake server: raw content ($value) must never be fetched by Slow Mail.")
        self._check_auth(req)
        path = re.sub(r"\('([^']+)'\)", r"/\1", path)
        segs = [s for s in path[len("/v1.0/"):].split("/") if s != ""]
        ctx = Ctx(immutable=req.prefs.get("idtype", "").lower() == "immutableid",
                  body_pref="text" if req.prefs.get("outlook.body-content-type") == "text" else "html", base=req.base)
        method = req.method
        resp: Optional[Resp] = None
        if segs == ["me"] and method == "GET":
            resp = self.get_me(req)
        elif len(segs) == 3 and segs[:2] == ["me", "mailFolders"] and method == "GET":
            resp = self.get_folder(req, segs[2])
        elif len(segs) == 5 and segs[:2] == ["me", "mailFolders"] and segs[3:] == ["messages", "delta"] and method == "GET":
            resp = self.delta(req, ctx, segs[2])
        elif len(segs) >= 3 and segs[:2] == ["me", "messages"]:
            resp = self.message_route(req, ctx, segs[2], segs[3:])
        if resp is None:
            raise ApiError(404, "ResourceNotFound" if method == "GET" else "ErrorInvalidRequest",
                           f"Unsupported {method} {req.path}")
        self._applied(req, ctx, resp)
        return resp

    @staticmethod
    def _applied(req: Req, ctx: Ctx, resp: Resp) -> None:
        resp.headers.append(("request-id", req.request_id))
        resp.headers.append(("client-request-id", req.client_request_id))
        if "odata.maxpagesize" in req.prefs and req.path.endswith("/delta"):
            resp.headers.append(("Preference-Applied", f"odata.maxpagesize={req.prefs['odata.maxpagesize']}"))
        if ctx.immutable:
            resp.headers.append(("Preference-Applied", 'IdType="ImmutableId"'))
        if "outlook.body-content-type" in req.prefs and ctx.body_pref == "text":
            resp.headers.append(("Preference-Applied", 'outlook.body-content-type="text"'))

    def _check_auth(self, req: Req) -> None:
        auth = req.headers.get("Authorization", "")
        parts = auth.split(None, 1)
        tok = self.access_tokens.get(parts[1].strip()) if len(parts) == 2 and parts[0].lower() == "bearer" else None
        if tok is None or tok["exp"] <= time.time():
            self.counters["graph_401"] += 1
            raise ApiError(401, "InvalidAuthenticationToken",
                           "Access token is empty, expired or invalid." if tok is None else "Access token has expired.",
                           [("WWW-Authenticate", 'Bearer realm="", authorization_uri="https://login.microsoftonline.com/common/oauth2/authorize"')])

    # ---- OAuth
    @staticmethod
    def oauth_error(error: str, desc: str, codes: int, status: int = 400) -> Resp:
        return Resp(status, {"error": error, "error_description": f"AADSTS{codes}: {desc}", "error_codes": [codes],
                             "timestamp": time.strftime("%Y-%m-%d %H:%M:%SZ", time.gmtime()),
                             "trace_id": str(uuid.uuid4()), "correlation_id": str(uuid.uuid4())})

    def oauth(self, req: Req, kind: str) -> Resp:
        if req.method != "POST":
            return self.oauth_error("invalid_request", "POST required.", 900144, 405)
        form = req.form()
        if kind == "devicecode":
            if not form.get("client_id") or not form.get("scope"):
                return self.oauth_error("invalid_request", "client_id and scope are required.", 900144)
            now = time.time()
            code = "fake-dc-" + uuid.uuid4().hex
            alphabet = "BCDFGHJKLMNPQRSTVWXZ"
            user_code = "".join(random.SystemRandom().choice(alphabet) for _ in range(9))
            dev = {"device_code": code, "user_code": user_code, "client_id": form["client_id"], "scope": form["scope"],
                   "issued": now, "expires": now + self.config.device_expires_in, "interval": self.config.device_interval,
                   "last_poll": None, "polls": 0, "status": "pending"}
            self.devices[code] = dev
            self.counters["device_codes_issued"] += 1
            interval = int(dev["interval"]) if float(dev["interval"]).is_integer() else dev["interval"]
            return Resp(200, {"device_code": code, "user_code": user_code,
                              "verification_uri": "https://microsoft.com/devicelogin",
                              "expires_in": self.config.device_expires_in, "interval": interval,
                              "message": f"To sign in, use a web browser to open the page https://microsoft.com/devicelogin "
                                         f"and enter the code {user_code} to authenticate."})
        grant = form.get("grant_type", "")
        if grant == "urn:ietf:params:oauth:grant-type:device_code":
            return self.poll_device(form)
        if grant == "refresh_token":
            return self.refresh(form)
        return self.oauth_error("unsupported_grant_type", "Unsupported grant_type.", 70003)

    def issue_tokens(self, scope: str, client_id: str, with_refresh: bool = True) -> Dict[str, Any]:
        ttl = self.config.token_ttl
        access = "fake-at-" + uuid.uuid4().hex
        self.access_tokens[access] = {"exp": time.time() + ttl, "scope": scope}
        granted = " ".join(s for s in scope.split() if s.lower() != "offline_access")
        out: Dict[str, Any] = {"token_type": "Bearer", "scope": granted, "expires_in": max(1, int(ttl)),
                               "ext_expires_in": max(1, int(ttl)), "access_token": access}
        if "offline_access" in scope.lower().split() and with_refresh:
            rt = "fake-rt-" + uuid.uuid4().hex
            self.refresh_tokens[rt] = {"scope": scope, "client_id": client_id, "revoked": False}
            out["refresh_token"] = rt
        if "openid" in scope.lower().split():
            claims = {"preferred_username": USER_UPN, "tid": TENANT_GUID, "oid": USER_ID, "name": "Slow Mail Test"}
            out["id_token"] = b64url(b'{"alg":"none","typ":"JWT"}') + "." + b64url(json.dumps(claims).encode()) + "."
        return out

    def poll_device(self, form: Dict[str, str]) -> Resp:
        dev = self.devices.get(form.get("device_code", ""))
        if dev is None:
            return self.oauth_error("bad_verification_code", "The device_code is not recognised.", 70020)
        if form.get("client_id") and form["client_id"] != dev["client_id"]:
            return self.oauth_error("invalid_client", "client_id does not match the device code request.", 70002)
        now = time.time()
        if dev["status"] == "expired" or now > dev["expires"]:
            return self.oauth_error("expired_token", "The device code has expired.", 70019)
        if dev["status"] == "declined":
            return self.oauth_error("authorization_declined", "The user declined the request.", 70001)
        dev["polls"] += 1
        last = dev["last_poll"]
        dev["last_poll"] = now
        if self.config.strict_interval and last is not None and now - last < dev["interval"] - 0.05:
            dev["interval"] += self.config.slow_down_increment
            return self.oauth_error("slow_down", "Polling too frequently; increase the interval.", 70016)
        if dev["status"] == "pending" and self.config.auto_approve_after > 0 and dev["polls"] > self.config.auto_approve_after:
            dev["status"] = "approved"
        if dev["status"] == "approved":
            del self.devices[dev["device_code"]]
            return Resp(200, self.issue_tokens(dev["scope"], dev["client_id"]))
        return self.oauth_error("authorization_pending", "Authorization is pending. Continue polling.", 70016)

    def refresh(self, form: Dict[str, str]) -> Resp:
        rt = form.get("refresh_token", "")
        info = self.refresh_tokens.get(rt)
        if info is None or info["revoked"]:
            return self.oauth_error("invalid_grant", "The refresh token is invalid, expired or revoked.", 70008)
        if form.get("scope"):
            granted = {s.lower() for s in info["scope"].split()}
            if not {s.lower() for s in form["scope"].split()} <= granted:
                return self.oauth_error("invalid_grant", "Requested scope exceeds the granted scope.", 65001)
        self.counters["refreshes"] += 1
        if self.config.strict_refresh:
            info["revoked"] = True  # rotation: the old refresh token is discarded
        return Resp(200, self.issue_tokens(info["scope"], info["client_id"]))

    # ---- Graph: me, folders
    def get_me(self, req: Req) -> Resp:
        full = {"id": USER_ID, "userPrincipalName": USER_UPN, "mail": USER_UPN, "displayName": "Slow Mail Test"}
        sel = req.q("$select")
        if sel:
            full = {k: v for k, v in full.items() if k.lower() in {s.strip().lower() for s in sel.split(",")} or k == "id"}
        return Resp(200, {"@odata.context": f"{req.base}/v1.0/$metadata#users/$entity", **full})

    def resolve_folder(self, name_or_id: str) -> Optional[str]:
        low = name_or_id.lower()
        for n in FOLDERS:
            if n == "archive" and not self.config.has_archive:
                continue
            if low == n or name_or_id == FOLDER_IDS[n]:
                return n
        return None

    def get_folder(self, req: Req, ident: str) -> Resp:
        name = self.resolve_folder(ident)
        if name is None:
            raise ApiError(404, "ErrorItemNotFound", "The specified object was not found in the store.")
        members = self.mailbox.members[name].values()
        full = {"id": FOLDER_IDS[name], "displayName": DISPLAY[name], "parentFolderId": ROOT_FOLDER_ID,
                "childFolderCount": 0, "unreadItemCount": sum(1 for m in members if not m.is_read),
                "totalItemCount": len(members), "isHidden": False}
        sel = req.q("$select")
        if sel:
            keep = {s.strip().lower() for s in sel.split(",")}
            full = {k: v for k, v in full.items() if k.lower() in keep or k == "id"}
        return Resp(200, {"@odata.context": f"{req.base}/v1.0/$metadata#users('{USER_ID}')/mailFolders/$entity", **full})

    # ---- Graph: delta
    def page_size(self, req: Req) -> int:
        raw = req.prefs.get("odata.maxpagesize")
        try:
            n = int(raw) if raw else self.config.default_page_size
        except ValueError:
            n = self.config.default_page_size
        return max(1, min(n, self.config.max_page_size))

    def delta(self, req: Req, ctx: Ctx, folder_ident: str) -> Resp:
        folder = self.resolve_folder(folder_ident)
        if folder is None:
            raise ApiError(404, "ErrorItemNotFound", "The specified folder could not be found.")
        mb, cfg = self.mailbox, self.config
        if req.q("$search") is not None:
            raise ApiError(400, "ErrorInvalidRestriction", "$search is not supported on delta.")
        orderby = req.q("$orderby")
        if orderby and orderby.strip().lower() != "receiveddatetime desc":
            raise ApiError(400, "ErrorInvalidRestriction", "Only $orderby=receivedDateTime desc is supported.")
        token = req.q("$skiptoken") or req.q("$deltatoken")
        link_path = f"{req.base}/v1.0/me/mailFolders/{folder_ident}/messages/delta"
        if token is not None:
            st = decode_state(token)
            gone = ApiError(cfg.delta_expiry_status, "SyncStateNotFound",
                            "The sync state generation is not found or has expired.",
                            [("Location", f"{link_path}?$deltatoken=")])
            if st is None or st.get("g") != mb.generation:
                raise gone
            if st.get("f") != folder:
                raise ApiError(400, "ErrorInvalidRequest", "Delta token belongs to a different folder.")
            if st["k"] == "d" and len(mb.logs[folder]) - st["p"] > cfg.delta_expiry_changes:
                raise gone
        else:
            select = [s for s in (req.q("$select") or "").split(",") if s.strip()] or None
            flt = req.q("$filter")
            lower = None
            if flt:
                m = re.match(r"^\s*receivedDateTime\s+(ge|gt)\s+(\S+)\s*$", flt)
                if not m or not ISO_RE.match(m.group(2)):
                    raise ApiError(400, "ErrorInvalidRestriction", "Only receivedDateTime ge|gt <datetime> is supported.")
                lower = [m.group(1), m.group(2)]
            st = {"k": "f", "f": folder, "g": mb.generation, "p": len(mb.logs[folder]), "c": None, "s": select,
                  "flt": lower, "ps": self.page_size(req)}
        # an explicit Prefer on the follow-up request wins over the size baked into the link
        ps = self.page_size(req) if "odata.maxpagesize" in req.prefs else st.get("ps", self.page_size(req))
        select = st.get("s")
        values: List[Dict[str, Any]] = []
        replay = st.get("rl") if cfg.delta_replay else None
        if replay:
            item = self._render_replay(replay, ctx, select)
            if item is not None:
                values.append(item)
        last: Optional[List[Any]] = None
        next_state: Optional[Dict[str, Any]] = None
        if st["k"] in ("f", "t"):
            asc = mb.sorted_keys(folder)
            lo = 0
            if st.get("flt"):
                op, val = st["flt"]
                lo = bisect.bisect_left(asc, (val, "")) if op == "ge" else bisect.bisect_right(asc, (val, "\U0010ffff"))
            hi = len(asc) if st["c"] is None else bisect.bisect_left(asc, tuple(st["c"]))
            j = hi - 1
            taken = 0
            cursor = st["c"]
            while j >= lo and taken < ps:
                msg = mb.messages[asc[j][1]]
                values.append(msg.render(ctx, select))
                last = ["m", msg.key]
                cursor = list(asc[j])
                j -= 1
                taken += 1
            if j >= lo:
                next_state = dict(st, k="f", c=cursor)
            elif cfg.trailing_empty_page and taken and st["k"] == "f":
                next_state = dict(st, k="t", c=[asc[lo][0], ""])  # tail page: nothing left, then deltaLink
            final = {"k": "d", "f": folder, "g": mb.generation, "p": st["p"], "s": select, "flt": st.get("flt"), "ps": st.get("ps", ps)}
        else:
            end = st["e"] if st["k"] == "s" else len(mb.logs[folder])
            start = st["p"]
            changes = mb.changes(folder, start, end, ctx, cfg.delta_partial_updates, cfg.immutable_gap)
            idx = st.get("i", 0)
            chunk = changes[idx:idx + ps]
            for kind, obj, fields in chunk:
                if kind == "removed":
                    values.append({"@odata.type": "#microsoft.graph.message", "id": obj, "@removed": {"reason": "deleted"}})
                    last = ["r", obj]
                elif kind == "partial":
                    values.append(obj.render_partial(ctx, fields))
                    last = ["m", obj.key]
                else:
                    values.append(obj.render(ctx, select))
                    last = ["m", obj.key]
            if idx + ps < len(changes):
                next_state = {"k": "s", "f": folder, "g": mb.generation, "p": start, "e": end, "i": idx + ps,
                              "s": select, "flt": st.get("flt"), "ps": st.get("ps", ps)}
            final = {"k": "d", "f": folder, "g": mb.generation, "p": end, "s": select, "flt": st.get("flt"),
                     "ps": st.get("ps", ps)}
        body: Dict[str, Any] = {"@odata.context": f"{req.base}/v1.0/$metadata#Collection(message)", "value": values}
        if next_state is not None:
            if last is not None:
                next_state["rl"] = last
            body["@odata.nextLink"] = f"{link_path}?$skiptoken={encode_state(next_state)}"
        else:
            body["@odata.deltaLink"] = f"{link_path}?$deltatoken={encode_state(final)}"
        return Resp(200, body)

    def _render_replay(self, rl: List[Any], ctx: Ctx, select: Optional[List[str]]) -> Optional[Dict[str, Any]]:
        if rl[0] == "r":
            return {"@odata.type": "#microsoft.graph.message", "id": rl[1], "@removed": {"reason": "deleted"}}
        msg = self.mailbox.messages.get(rl[1])
        return msg.render(ctx, select) if msg else None

    # ---- Graph: messages
    def message_route(self, req: Req, ctx: Ctx, ident: str, rest: List[str]) -> Optional[Resp]:
        msg = self.mailbox.find(ident, ctx.immutable)
        if msg is None:
            raise ApiError(404, "ErrorItemNotFound", "The specified object was not found in the store.")
        method = req.method
        select = [s for s in (req.q("$select") or "").split(",") if s.strip()] or None
        ctxurl = f"{req.base}/v1.0/$metadata#users('{USER_ID}')/messages"
        if not rest:
            if method == "GET":
                return Resp(200, {"@odata.context": ctxurl + "/$entity", **msg.render(ctx, select)})
            if method == "PATCH":
                return self.patch(req, ctx, msg, ctxurl)
            if method == "DELETE":
                self.counters["delete_calls"] += 1
                self.mailbox.move(msg, "deleteditems")
                return Resp(204)
        elif rest[0] == "move" and method == "POST":
            body = req.json()
            dest_ident = body.get("destinationId") if isinstance(body, dict) else None
            if not dest_ident:
                raise ApiError(400, "ErrorInvalidRequest", "destinationId is required.")
            dest = self.resolve_folder(str(dest_ident))
            if dest is None:
                raise ApiError(404, "ErrorFolderNotFound", "The specified folder could not be found in the store.")
            self.mailbox.move(msg, dest)  # no-op when already there
            return Resp(201, {"@odata.context": ctxurl + "/$entity", **msg.render(ctx)})
        elif rest[0] == "attachments" and method == "GET":
            if len(rest) == 1:
                items, served = msg.render_attachments(select, self.config.doc_style_odata_type)
                self.counters["content_bytes_served"] += served
                return Resp(200, {"@odata.context": f"{ctxurl}('{ident}')/attachments", "value": items})
            if len(rest) == 2:
                items, served = msg.render_attachments(select, self.config.doc_style_odata_type)
                for item, raw in zip(items, msg.attachments):
                    if raw["id"] == rest[1]:
                        self.counters["content_bytes_served"] += 1 if "contentBytes" in item else 0
                        return Resp(200, {"@odata.context": f"{ctxurl}('{ident}')/attachments/$entity", **item})
                raise ApiError(404, "ErrorItemNotFound", "The specified attachment was not found.")
        return None

    def patch(self, req: Req, ctx: Ctx, msg: Message, ctxurl: str) -> Resp:
        body = req.json()
        if not isinstance(body, dict):
            raise ApiError(400, "ErrorInvalidRequest", "Body must be a JSON object.")
        match = req.headers.get("If-Match")
        if match and match != "*" and match != f'W/"{msg.change_key}"':
            raise ApiError(412, "ErrorIrresolvableConflict", "The change key passed does not match the current item.")
        for k in body:
            if k in READ_ONLY_FIELDS:
                raise ApiError(400, "ErrorInvalidRequest", f"Property {k} can only be set on draft messages.")
        changed = set()
        if "flag" in body:
            flag = body["flag"]
            if not isinstance(flag, dict) or flag.get("flagStatus") not in FLAG_VALUES:
                raise ApiError(400, "ErrorInvalidRequest", "flag.flagStatus must be notFlagged, flagged or complete.")
            if flag.get("dueDateTime") and not flag.get("startDateTime"):
                raise ApiError(400, "ErrorInvalidRequest", "startDateTime is required when dueDateTime is set.")
            new_flag = {"flagStatus": flag["flagStatus"]}
            for k in ("startDateTime", "dueDateTime", "completedDateTime"):
                if flag.get(k) and flag["flagStatus"] != "notFlagged":
                    new_flag[k] = flag[k]
            if flag["flagStatus"] == "complete" and "completedDateTime" not in new_flag:
                new_flag["completedDateTime"] = {"dateTime": now_iso()[:-1] + ".0000000", "timeZone": "UTC"}
            if new_flag != msg.flag:
                msg.flag = new_flag
                changed.add("flag")
        if "isRead" in body:
            if not isinstance(body["isRead"], bool):
                raise ApiError(400, "ErrorInvalidRequest", "isRead must be a boolean.")
            if body["isRead"] != msg.is_read:
                msg.is_read = body["isRead"]
                changed.add("isRead")
        if changed:
            self.mailbox.update(msg, changed)
        return Resp(200, {"@odata.context": ctxurl + "/$entity", **msg.render(ctx)})

    # ---- control API
    def control(self, req: Req) -> Resp:
        name = req.path[len("/__control/"):].strip("/")
        method = req.method
        mb = self.mailbox
        if method == "GET":
            if name == "state":
                return Resp(200, self.state())
            if name == "requests":
                return Resp(200, list(self.log))
            if name == "config":
                return Resp(200, dataclasses.asdict(self.config))
            raise ApiError(404, "unknown control endpoint")
        if method != "POST":
            raise ApiError(405, "method not allowed")
        body = req.json()
        if not isinstance(body, dict):
            raise ApiError(400, "JSON object expected")
        if name == "reset":
            self.reset(bool(body.get("keep_auth")))
            return Resp(200, {"ok": True})
        if name == "clear_log":
            self.log.clear()
            by = self.counters
            for k in list(by):
                by[k] = {} if k == "by_path" else 0
            return Resp(200, {"ok": True})
        if name == "load":
            replace = not body.get("append", False)
            if "path" in body:
                n = self.load_file(str(body["path"]), replace)
            elif "generate" in body:
                n = self.load_data(generate_fixture.generate(int(body["generate"]), int(body.get("seed", 42))), replace)
            else:
                raise ApiError(400, "need 'path' or 'generate'")
            return Resp(200, {"ok": True, "loaded": n, "total": len(mb.messages)})
        if name in ("approve_device", "decline_device", "expire_device"):
            dev = self._find_device(body.get("user_code"))
            dev["status"] = {"approve_device": "approved", "decline_device": "declined", "expire_device": "expired"}[name]
            return Resp(200, {"ok": True, "user_code": dev["user_code"]})
        if name == "revoke_refresh":
            for info in self.refresh_tokens.values():
                info["revoked"] = True
            if body.get("also_access"):
                self.access_tokens.clear()
            return Resp(200, {"ok": True})
        if name == "expire_tokens":
            self.access_tokens.clear()
            return Resp(200, {"ok": True})
        if name == "expire_delta":
            mb.generation += 1
            return Resp(200, {"ok": True, "generation": mb.generation})
        if name == "fault":
            rule = {"match": str(body["match"]), "method": body.get("method"), "status": int(body.get("status", 500)),
                    "count": int(body.get("count", 1)), "body": body.get("body"), "delay": body.get("delay", 0),
                    "headers": body.get("headers")}
            if "retry_after" in body:
                rule["retry_after"] = body["retry_after"]  # null => omit the header
            self.faults.append(rule)
            return Resp(200, {"ok": True, "pending_faults": len(self.faults)})
        if name == "drop":
            self.drops.append({"match": str(body["match"]), "method": body.get("method"),
                               "count": int(body.get("count", 1)), "after": bool(body.get("after", False))})
            return Resp(200, {"ok": True})
        if name == "clear_faults":
            self.faults.clear()
            self.drops.clear()
            return Resp(200, {"ok": True})
        if name == "config":
            self.config.update(body)
            return Resp(200, dataclasses.asdict(self.config))
        if name == "folders":
            if "archive" in body:
                self.config.has_archive = bool(body["archive"])
            return Resp(200, {"ok": True, "archive": self.config.has_archive})
        if name == "external":
            return Resp(200, self.external(body))
        raise ApiError(404, "unknown control endpoint")

    def _find_device(self, user_code: Optional[str]) -> Dict[str, Any]:
        pending = [d for d in self.devices.values()]
        if user_code:
            norm = str(user_code).replace("-", "").upper()
            for d in pending:
                if d["user_code"].replace("-", "").upper() == norm:
                    return d
            raise ApiError(404, "unknown user_code")
        if not pending:
            raise ApiError(404, "no device code outstanding")
        return pending[-1]

    def external(self, body: Dict[str, Any]) -> Dict[str, Any]:
        """Simulate another Outlook client changing the mailbox."""
        op = body.get("op")
        mb = self.mailbox
        if op == "add":
            if "message" in body:
                return {"ok": True, "ids": [self.add_message(body["message"], body.get("folder")).key]}
            count = int(body.get("count", 1))
            data = generate_fixture.generate(count, int(body.get("seed", 99)), now=now_iso(), days=1,
                                             id_tag=f"{mb.auto_id:04x}")
            return {"ok": True, "ids": [self.add_message(m, body.get("folder")).key for m in data["messages"]]}
        ident = body.get("id")
        msg = mb.find_any(str(ident)) if ident else None
        if msg is None:
            raise ApiError(404, f"message not found: {ident}")
        if op == "move":
            dest = self.resolve_folder(str(body.get("to", "")))
            if dest is None:
                raise ApiError(400, "unknown destination folder")
            mb.move(msg, dest)
        elif op == "delete":
            if body.get("hard"):
                mb.hard_delete(msg)
                return {"ok": True, "id": msg.key}
            mb.move(msg, "deleteditems")
        elif op in ("flag", "unflag"):
            msg.flag = {"flagStatus": "flagged" if op == "flag" else "notFlagged"}
            mb.update(msg, {"flag"})
        elif op in ("read", "unread"):
            msg.is_read = op == "read"
            mb.update(msg, {"isRead"})
        else:
            raise ApiError(400, f"unknown op: {op}")
        return {"ok": True, "id": msg.key, "mutable_id": msg.mut, "folder": msg.folder}

    def state(self) -> Dict[str, Any]:
        mb = self.mailbox
        folders = {n: [k for _, k in reversed(mb.sorted_keys(n))] for n in FOLDERS}
        messages = {k: {"subject": m.props.get("subject", ""), "flagStatus": m.flag.get("flagStatus"),
                        "parentFolder": m.folder, "isRead": m.is_read, "mutableId": m.mut}
                    for k, m in mb.messages.items()}
        counters = dict(self.counters)
        counters["by_path"] = dict(self.counters["by_path"])
        return {"folders": folders, "messages": messages, "counters": counters, "generation": mb.generation,
                "config": dataclasses.asdict(self.config)}


# --------------------------------------------------------------------------- HTTP plumbing
class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server: "FakeGraphServer"

    def log_message(self, fmt: str, *args: Any) -> None:  # silence default access log
        pass

    def _read_body(self) -> bytes:
        if "chunked" in self.headers.get("Transfer-Encoding", "").lower():
            data = b""
            while True:
                size = int(self.rfile.readline().split(b";")[0].strip() or b"0", 16)
                if size == 0:
                    self.rfile.readline()
                    return data
                data += self.rfile.read(size)
                self.rfile.readline()
        length = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(length) if length else b""

    def _handle(self) -> None:
        try:
            body = self._read_body()
        except (ValueError, OSError):
            self.close_connection = True
            return
        host = self.headers.get("Host") or f"127.0.0.1:{self.server.server_address[1]}"
        req = Req(self.command, self.path, self.headers, body, host)
        app = self.server.app
        resp, drop_after = app.handle(req)
        if app.verbose and not req.path.startswith("/__control/"):
            sys.stderr.write(f"{req.method} {req.path}{'?' + urlsplit(req.target).query if req.query else ''} -> "
                             f"{'DROP' if resp is None else resp.status}\n")
            sys.stderr.flush()
        if resp is None or drop_after:
            self.close_connection = True
            try:
                self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            return
        payload = b"" if resp.body is None else json.dumps(resp.body, ensure_ascii=False).encode("utf-8")
        try:
            self.send_response(resp.status)
            if resp.status != 204:
                self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(payload)))
            for k, v in resp.headers:
                self.send_header(k, v)
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            self.close_connection = True

    do_GET = do_POST = do_PATCH = do_DELETE = do_PUT = _handle


class FakeGraphServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True
    request_queue_size = 128

    def __init__(self, address: Tuple[str, int], app: App):
        super().__init__(address, Handler)
        self.app = app


def make_server(config: Optional[Config] = None, host: str = "127.0.0.1", port: int = 0,
                verbose: bool = False) -> FakeGraphServer:
    return FakeGraphServer((host, port), App(config or Config(), verbose))


def parse_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(description="Fake Microsoft identity platform + Graph v1.0 server for Slow Mail")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8765, help="0 = pick a free port and print 'LISTENING <port>'")
    ap.add_argument("--verbose", action="store_true", help="one line per request on stderr (never prints tokens)")
    ap.add_argument("--load", help="fixture JSON to load at startup")
    ap.add_argument("--generate", type=int, default=0, help="generate N inbox messages at startup")
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--token-ttl", type=float, default=3600.0)
    ap.add_argument("--delta-expiry-changes", type=int, default=100000)
    ap.add_argument("--auto-approve-after", type=int, default=0, metavar="N",
                    help="approve a device code automatically once it has been polled N times (N+1th poll succeeds)")
    ap.add_argument("--device-interval", type=float, default=5.0)
    ap.add_argument("--device-expires-in", type=int, default=900)
    ap.add_argument("--no-slow-down", action="store_true", help="never answer slow_down")
    ap.add_argument("--lenient-refresh", action="store_true", help="do not invalidate used refresh tokens")
    ap.add_argument("--no-archive", action="store_true", help="mailbox without an archive folder")
    ap.add_argument("--delta-partial-updates", action=argparse.BooleanOptionalAction, default=True,
                    help="updated messages in delta carry only id + changed fields + changeKey (default on)")
    ap.add_argument("--delta-replay", action="store_true", help="repeat the last item of each delta page on the next page")
    ap.add_argument("--delta-expiry-status", type=int, choices=(400, 410), default=410)
    ap.add_argument("--immutable-gap", action="store_true", help="emulate the Graph immutable-id delta known issue")
    ap.add_argument("--trailing-empty-page", action="store_true")
    ap.add_argument("--exit-on-stdin-eof", action="store_true", help="exit when stdin closes (parent died)")
    return ap.parse_args(argv)


def main(argv: Optional[List[str]] = None) -> int:
    args = parse_args(argv)
    cfg = Config(token_ttl=args.token_ttl, delta_expiry_changes=args.delta_expiry_changes,
                 auto_approve_after=args.auto_approve_after, device_interval=args.device_interval,
                 device_expires_in=args.device_expires_in, strict_interval=not args.no_slow_down,
                 strict_refresh=not args.lenient_refresh, has_archive=not args.no_archive,
                 delta_partial_updates=args.delta_partial_updates, delta_replay=args.delta_replay,
                 delta_expiry_status=args.delta_expiry_status, immutable_gap=args.immutable_gap,
                 trailing_empty_page=args.trailing_empty_page)
    srv = make_server(cfg, args.host, args.port, args.verbose)
    try:
        if args.load:
            srv.app.load_file(args.load)
        elif args.generate:
            srv.app.load_data(generate_fixture.generate(args.generate, args.seed))
    except (OSError, ValueError) as e:
        sys.stderr.write(f"fixture error: {e}\n")
        return 2
    print(f"LISTENING {srv.server_address[1]}", flush=True)
    signal.signal(signal.SIGTERM, lambda *_: threading.Thread(target=srv.shutdown, daemon=True).start())
    if args.exit_on_stdin_eof:
        def watch() -> None:
            try:
                sys.stdin.read()
            finally:
                os._exit(0)
        threading.Thread(target=watch, daemon=True).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
