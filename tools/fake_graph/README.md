# Fake Microsoft Graph server

`server.py` emulates the subset of the Microsoft identity platform (v2.0 device-code OAuth) and
Microsoft Graph v1.0 mail API that Slow Mail uses, on **one port**, with a JSON control API for loading
fixtures and injecting faults. Python 3 standard library only; no pip packages.
Behaviour follows `docs/GRAPH_CONTRACT.md` (section 11 is the fidelity checklist).

Files: `server.py` (server), `generate_fixture.py` (fixture generator), `test_server.py` (unit tests).

## Run it for the desktop simulator

```sh
python3 tools/fake_graph/server.py --port 8765 --generate 50 --auto-approve-after 2
```

Point the app at it: authority `http://127.0.0.1:8765`, tenant `common`, Graph base `http://127.0.0.1:8765/v1.0`.
With `--auto-approve-after 2` the third poll of a device code succeeds, so sign-in needs no manual step.
Otherwise approve by hand with the user code shown by the app:

```sh
curl -s -XPOST localhost:8765/__control/approve_device -d '{"user_code":"ABCDEFGHJ"}'   # omit user_code: newest pending code
```

`--port 0` picks a free port and prints `LISTENING <port>` as the first stdout line (flushed); this is what the
C++ contract tests use. `--exit-on-stdin-eof` makes the server exit if its parent dies.

### Options

| Option | Default | Meaning |
|---|---|---|
| `--host`, `--port` | 127.0.0.1, 8765 | listen address; port 0 = free port |
| `--load PATH` / `--generate N [--seed S]` | none | load a fixture or generate N inbox messages at start |
| `--token-ttl SECONDS` | 3600 | access token lifetime |
| `--delta-expiry-changes N` | 100000 | delta token older than N changes returns an error |
| `--delta-expiry-status {410,400}` | 410 | status for an expired/unknown delta token (code is always `SyncStateNotFound`) |
| `--delta-partial-updates` / `--no-delta-partial-updates` | on | updated messages in delta carry only `id` + changed fields + `changeKey`; off = full projection |
| `--delta-replay` | off | repeat the last item of each delta page at the start of the next (legal duplicates) |
| `--immutable-gap` | off | known Graph issue: a message moved out and back in shows only `@removed` |
| `--trailing-empty-page` | off | final delta page is empty and carries the `deltaLink` |
| `--auto-approve-after N` | 0 (off) | the (N+1)th poll of a device code succeeds |
| `--device-interval S`, `--device-expires-in S` | 5, 900 | device-code polling interval / lifetime |
| `--no-slow-down` | off | never answer `slow_down` |
| `--lenient-refresh` | off | refresh does not invalidate the used refresh token (default rotates, as Entra tells apps to expect) |
| `--no-archive` | off | mailbox without an archive folder |
| `--verbose` | off | one stderr line per request (`METHOD path?query -> status`); tokens are never printed |

All of these except `--port`/`--host`/`--load`/`--generate` can also be changed at runtime with `POST /__control/config`
(keys: `token_ttl`, `delta_expiry_changes`, `delta_expiry_status`, `delta_partial_updates`, `delta_replay`,
`immutable_gap`, `trailing_empty_page`, `device_interval`, `device_expires_in`, `auto_approve_after`,
`strict_interval`, `slow_down_increment`, `strict_refresh`, `has_archive`, `doc_style_odata_type`,
`default_page_size`). `reset` restores the startup values.

## What it emulates

* **OAuth**: `POST /{tenant}/oauth2/v2.0/devicecode` (form `client_id`, `scope`); `POST /{tenant}/oauth2/v2.0/token`
  with the device-code grant (`authorization_pending`, `slow_down` when polled faster than `interval`,
  `expired_token`, `authorization_declined`, `bad_verification_code`) or `refresh_token` grant (rotates tokens;
  `invalid_grant` if revoked). `refresh_token` is only issued when `offline_access` was requested. Error bodies carry
  `error`, `error_description`, `error_codes`, `timestamp`, `trace_id`, `correlation_id`. The first poll is never
  rate-limited.
* **Graph auth**: every `/v1.0/*` call needs `Authorization: Bearer <valid unexpired token>`, else 401
  `InvalidAuthenticationToken`.
* **Ids**: each message has an immutable id and a mutable id that changes on every move. Requests with
  `Prefer: IdType="ImmutableId"` see/accept immutable ids; requests without it see/accept mutable ids, so a move
  returns a new id and the old id then 404s (`ErrorItemNotFound`). `Prefer` may be comma-joined or repeated.
* **Endpoints**: `GET /v1.0/me`; `GET /v1.0/me/mailFolders/{inbox|archive|deleteditems|id}`;
  `GET .../mailFolders/{folder}/messages/delta` (works for all three folders); `GET|PATCH|DELETE /v1.0/me/messages/{id}`;
  `POST .../move` (201 + moved message); `GET .../attachments`, `GET .../attachments/{aid}`.
  All JSON responses carry `@odata.context`. `$select` projects (always keeping `@odata.type`, `@odata.etag`, `id`).
  `Prefer: outlook.body-content-type="text"` converts stored HTML to text crudely (tags stripped, `\r\n`) and is
  confirmed with `Preference-Applied`.
* **Delta**: a real per-folder change log. First call = full enumeration (newest first) in pages of
  `Prefer: odata.maxpagesize=N` (default 10) linked by `@odata.nextLink?$skiptoken=...`; the last page carries
  `@odata.deltaLink?$deltatoken=...`. Following a deltaLink returns only changes: added/moved-in messages in full,
  updated messages as partial updates (see above), removed/moved-out/deleted ones as
  `{"@odata.type":"#microsoft.graph.message","id":...,"@removed":{"reason":"deleted"}}`. In mutable-id mode the removal
  uses the id the client knew. Tokens encode folder, log position and generation; garbage, wrong generation
  (`reset`, `load`, `expire_delta`) or too old (`--delta-expiry-changes`) gives `410 SyncStateNotFound` with a `Location`
  header. Supports `$select`, `$orderby=receivedDateTime desc`, `$filter=receivedDateTime ge|gt <iso>` on the initial request; anything else is 400.
* **Attachments**: `fileAttachment`, `itemAttachment`, `referenceAttachment`. With `$select` the response contains only
  the selected metadata; **without `$select`, `contentBytes` is included for file attachments (as real Graph does)** and
  `content_bytes_served` increments, so a test can assert the client always sends `$select`. Any `.../$value` GET
  returns 403 and increments `forbidden_fetch`. `hasAttachments` is false when only inline attachments exist.
* **PATCH**: `flag.flagStatus` (`notFlagged|flagged|complete`; `dueDateTime` without `startDateTime` is 400),
  `isRead`. Read-only properties are 400. `changeKey` changes only when something actually changed. `If-Match` mismatch is 412.
* **Move to a missing folder**: `destinationId` unknown, or `archive` after `POST /__control/folders {"archive": false}`
  -> 404 `ErrorFolderNotFound` (distinct from `ErrorItemNotFound`). Moving to the folder the message is already in is a no-op 201.
* `DELETE /v1.0/me/messages/{id}` soft-deletes (moves to Deleted Items) and increments `delete_calls`.

## Control API (`/__control/`, JSON, no auth, not logged or counted)

| Request | Effect |
|---|---|
| `POST reset` `{"keep_auth":false}` | empty mailbox, drop faults/log/counters/tokens, restore startup config, invalidate delta tokens |
| `POST clear_log` | clear the request log and counters only |
| `POST load` `{"path": fixture}` or `{"generate": N, "seed": S}` (+ `"append": true`) | replace (or append to) the mailbox; replacing invalidates delta tokens |
| `POST approve_device` / `decline_device` / `expire_device` `{"user_code"}` | resolve a device code (no `user_code` = newest) |
| `POST revoke_refresh` `{"also_access":false}` | all refresh tokens now fail with `invalid_grant` |
| `POST expire_tokens` | forget all access tokens (Graph answers 401; the client must refresh) |
| `POST expire_delta` | bump the generation: every delta token now gets 410 |
| `POST fault` `{"match","method","status","retry_after","count","body","headers","delay"}` | answer the next `count` (default 1; negative = unlimited) matching requests with `status`. `match` is a substring of path+query. `retry_after` defaults to 1 for 429/503; `null` omits the header. Applies to OAuth endpoints too |
| `POST drop` `{"match","method","count","after"}` | close the connection without answering. `"after": true` executes the request first and then drops (lost response) |
| `POST clear_faults` | remove pending faults and drops |
| `POST external` | simulate another Outlook client (below) |
| `POST config` `{...}` / `GET config` | change / read runtime options |
| `POST folders` `{"archive": false}` | remove / restore the archive folder |
| `GET state` | `{folders:{inbox:[ids],archive:[..],deleteditems:[..]}, messages:{id:{subject,flagStatus,parentFolder,isRead,mutableId}}, counters:{requests,forbidden_fetch,content_bytes_served,delete_calls,graph_401,refreshes,device_codes_issued,by_path}, generation, config}`. Ids are immutable ids; folder lists are newest first. `by_path` keys look like `POST /v1.0/me/messages/{id}/move` |
| `GET requests` | last 200 Graph/OAuth requests: `seq, method, path, query, headers{Prefer,Content-Type,Accept,User-Agent,If-Match,Authorization:"Bearer <redacted>"}, body_length, status` (`status` 0 = dropped) |

`external` ops (ids may be immutable or current mutable ids):
`{"op":"move","id":..,"to":"archive"}`, `{"op":"delete","id":..}` (soft, to Deleted Items) or with `"hard":true`
(gone), `{"op":"flag"|"unflag"|"read"|"unread","id":..}`,
`{"op":"add","message":{...fixture message...},"folder":"inbox"}` or `{"op":"add","count":N,"seed":S}` (new generated mail,
received now). Each change lands in the folder change logs, so the next delta shows it.

## Fixture format

```json
{
  "format": "slowmail-fake-graph-fixture/1",
  "messages": [
    {
      "id": "optional immutable id (generated if absent)",
      "folder": "inbox | archive | deleteditems   (default inbox)",
      "conversationId": "...", "internetMessageId": "...",
      "subject": "...", "from": {"emailAddress": {"name": "..", "address": ".."}},
      "receivedDateTime": "2026-10-20T01:23:45Z",
      "isRead": false, "flag": {"flagStatus": "notFlagged"},
      "body": {"contentType": "html | text", "content": "..."},
      "attachments": [
        {"@odata.type": "#microsoft.graph.fileAttachment | itemAttachment | referenceAttachment",
         "id": "..", "name": "..", "contentType": "..|null", "size": 123, "isInline": false, "contentBytes": "..."}
      ]
    }
  ]
}
```

A bare JSON list of messages also works. Any other Graph message property (`toRecipients`, `importance`, ...) is passed
through unchanged. Defaults: `bodyPreview` = first 255 chars of the text body, `hasAttachments` = any non-inline
attachment, `sender` mirrors `from`; `from`/`address` may be missing on purpose (malformed senders). `contentBytes` is
stored but only served when the client omits `$select` (and counted).

`generate_fixture.py` writes deterministic fixtures (same `--seed` => identical bytes):

```sh
python3 tools/fake_graph/generate_fixture.py --count 1000 --seed 42 --out tests/contract/fixtures/inbox_1000.json
python3 tools/fake_graph/generate_fixture.py --small --out tests/contract/fixtures/inbox_small.json
```

`inbox_1000.json`: mixed html/text bodies (script, tracking pixels, remote CSS, nested quotes), 4% subjects of 400+ chars
(emoji, RTL, CJK, combining marks), 2% empty subjects, 1% control characters, malformed senders (missing address 2%,
no `from` 1%), exactly 10% flagged, exactly 20% with attachments (PDF, DOCX, PNG, inline images, item and reference
attachments; one 10 000-char name, one `size: -1`), timestamps over 90 days. `inbox_small.json` has 12 hand-readable
messages with ids `small-01-...` to `small-12-...`, one per case (plain, HTML+script+tracker, pre-flagged, empty subject,
long Unicode subject, missing sender address, PDF+DOCX+PNG, inline+item+reference, attachment extremes, nested quotes,
inline-only, 89 days old with no `from`).

## Tests

```sh
python3 -I tools/fake_graph/test_server.py          # unit tests, server started in-process on port 0
cmake -S . -B build-contract -G Ninja -DSLOWMAIL_BUILD_UI=OFF -DSLOWMAIL_BUILD_CLI=OFF
cmake --build build-contract --target contract_tests && ./build-contract/tests/contract/contract_tests
```

The C++ suite spawns `server.py --port 0` itself (set `SLOWMAIL_PYTHON` to pick the interpreter).
