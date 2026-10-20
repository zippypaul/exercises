#!/usr/bin/env python3
"""Unit tests for the fake Graph server.  Run:  python3 -I tools/fake_graph/test_server.py

Starts the server in-process on port 0 and exercises every endpoint and fault.
"""
from __future__ import annotations

import http.client
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.parse
import urllib.request
from typing import Any, Dict, List, Optional, Tuple

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import generate_fixture  # noqa: E402
import server as fake  # noqa: E402

GRANT = "urn:ietf:params:oauth:grant-type:device_code"
IMM = {"Prefer": 'IdType="ImmutableId"'}
FIXTURE_DIR = os.path.join(os.path.dirname(os.path.dirname(HERE)), "tests", "contract", "fixtures")
NET_ERRORS = (urllib.error.URLError, http.client.HTTPException, ConnectionError)
Resp = Tuple[int, Dict[str, str], Any]


class Base(unittest.TestCase):
    srv: fake.FakeGraphServer
    base: str

    @classmethod
    def setUpClass(cls) -> None:
        cls.srv = fake.make_server(fake.Config(device_interval=0.3), port=0)
        cls.base = f"http://127.0.0.1:{cls.srv.server_address[1]}"
        cls.thread = threading.Thread(target=cls.srv.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls) -> None:
        cls.srv.shutdown()
        cls.srv.server_close()

    def setUp(self) -> None:
        self.ctl("reset")
        self.token: Optional[str] = None

    # -- http helpers
    def raw(self, method: str, path: str, body: Any = None, headers: Optional[Dict[str, str]] = None,
            form: bool = False) -> Resp:
        data = None
        hdrs = dict(headers or {})
        if body is not None:
            if form:
                data = urllib.parse.urlencode(body).encode()
                hdrs["Content-Type"] = "application/x-www-form-urlencoded"
            else:
                data = json.dumps(body).encode()
                hdrs["Content-Type"] = "application/json"
        url = path if path.startswith("http") else self.base + path
        req = urllib.request.Request(url, data=data, method=method, headers=hdrs)
        try:
            with urllib.request.urlopen(req, timeout=10) as r:
                status, h, payload = r.status, {k.lower(): v for k, v in r.headers.items()}, r.read()
        except urllib.error.HTTPError as e:
            status, h, payload = e.code, {k.lower(): v for k, v in e.headers.items()}, e.read()
        return status, h, (json.loads(payload) if payload else None)

    def ctl(self, name: str, body: Optional[Dict[str, Any]] = None) -> Any:
        if body is None and name in ("state", "requests", "config"):
            return self.raw("GET", f"/__control/{name}")[2]
        st, _, out = self.raw("POST", f"/__control/{name}", body or {})
        self.assertEqual(st, 200, out)
        return out

    def sign_in(self, scope: str = "offline_access User.Read Mail.ReadWrite") -> Dict[str, Any]:
        st, _, dc = self.raw("POST", "/common/oauth2/v2.0/devicecode", {"client_id": "cid", "scope": scope}, form=True)
        self.assertEqual(st, 200)
        self.ctl("approve_device", {"user_code": dc["user_code"]})
        st, _, tok = self.raw("POST", "/common/oauth2/v2.0/token",
                              {"grant_type": GRANT, "client_id": "cid", "device_code": dc["device_code"]}, form=True)
        self.assertEqual(st, 200, tok)
        self.token = tok["access_token"]
        return tok

    def g(self, method: str, path: str, body: Any = None, headers: Optional[Dict[str, str]] = None) -> Resp:
        h = dict(headers or {})
        if self.token:
            h["Authorization"] = "Bearer " + self.token
        return self.raw(method, path, body, h)

    def load(self, n: int = 10, seed: int = 1) -> List[str]:
        self.ctl("load", {"generate": n, "seed": seed})
        return list(self.ctl("state")["folders"]["inbox"])

    def delta_all(self, url: str, headers: Optional[Dict[str, str]] = None) -> Tuple[List[dict], List[List[dict]], str]:
        pages: List[List[dict]] = []
        while True:
            st, _, body = self.g("GET", url, headers=headers)
            self.assertEqual(st, 200, body)
            pages.append(body["value"])
            if "@odata.nextLink" in body:
                self.assertNotIn("@odata.deltaLink", body)
                url = body["@odata.nextLink"]
            else:
                return [v for p in pages for v in p], pages, body["@odata.deltaLink"]


class OAuthTests(Base):
    def test_device_code_flow_end_to_end(self) -> None:
        st, _, dc = self.raw("POST", "/common/oauth2/v2.0/devicecode",
                             {"client_id": "cid", "scope": "offline_access Mail.ReadWrite"}, form=True)
        self.assertEqual(st, 200)
        for k in ("device_code", "user_code", "verification_uri", "expires_in", "interval", "message"):
            self.assertIn(k, dc)
        self.assertIn(dc["user_code"], dc["message"])
        poll = {"grant_type": GRANT, "client_id": "cid", "device_code": dc["device_code"]}
        st, _, err = self.raw("POST", "/common/oauth2/v2.0/token", poll, form=True)
        self.assertEqual((st, err["error"]), (400, "authorization_pending"))
        self.ctl("approve_device", {"user_code": dc["user_code"]})
        time.sleep(dc["interval"] + 0.1)
        st, _, tok = self.raw("POST", "/common/oauth2/v2.0/token", poll, form=True)
        self.assertEqual(st, 200)
        self.assertEqual(tok["token_type"], "Bearer")
        self.assertEqual(tok["scope"], "Mail.ReadWrite")
        for k in ("access_token", "refresh_token", "expires_in"):
            self.assertIn(k, tok)
        self.token = tok["access_token"]
        self.assertEqual(self.g("GET", "/v1.0/me")[0], 200)

    def test_missing_params_and_bad_codes(self) -> None:
        st, _, err = self.raw("POST", "/common/oauth2/v2.0/devicecode", {"client_id": "cid"}, form=True)
        self.assertEqual((st, err["error"]), (400, "invalid_request"))
        st, _, err = self.raw("POST", "/common/oauth2/v2.0/token", {"grant_type": GRANT, "device_code": "nope"}, form=True)
        self.assertEqual(err["error"], "bad_verification_code")
        st, _, err = self.raw("POST", "/common/oauth2/v2.0/token", {"grant_type": "password"}, form=True)
        self.assertEqual(err["error"], "unsupported_grant_type")

    def test_no_refresh_token_without_offline_access(self) -> None:
        tok = self.sign_in(scope="Mail.ReadWrite")
        self.assertNotIn("refresh_token", tok)

    def test_slow_down_expired_declined(self) -> None:
        _, _, dc = self.raw("POST", "/common/oauth2/v2.0/devicecode", {"client_id": "cid", "scope": "offline_access"}, form=True)
        poll = {"grant_type": GRANT, "client_id": "cid", "device_code": dc["device_code"]}
        self.assertEqual(self.raw("POST", "/common/oauth2/v2.0/token", poll, form=True)[2]["error"], "authorization_pending")
        self.assertEqual(self.raw("POST", "/common/oauth2/v2.0/token", poll, form=True)[2]["error"], "slow_down")
        self.ctl("expire_device", {"user_code": dc["user_code"]})
        self.assertEqual(self.raw("POST", "/common/oauth2/v2.0/token", poll, form=True)[2]["error"], "expired_token")
        _, _, dc2 = self.raw("POST", "/common/oauth2/v2.0/devicecode", {"client_id": "cid", "scope": "offline_access"}, form=True)
        self.ctl("decline_device", {"user_code": dc2["user_code"]})
        poll2 = dict(poll, device_code=dc2["device_code"])
        self.assertEqual(self.raw("POST", "/common/oauth2/v2.0/token", poll2, form=True)[2]["error"], "authorization_declined")

    def test_device_code_natural_expiry(self) -> None:
        self.ctl("config", {"device_expires_in": 0})
        _, _, dc = self.raw("POST", "/common/oauth2/v2.0/devicecode", {"client_id": "cid", "scope": "offline_access"}, form=True)
        time.sleep(0.05)
        err = self.raw("POST", "/common/oauth2/v2.0/token",
                       {"grant_type": GRANT, "client_id": "cid", "device_code": dc["device_code"]}, form=True)[2]
        self.assertEqual(err["error"], "expired_token")

    def test_auto_approve_after_n_polls(self) -> None:
        self.ctl("config", {"auto_approve_after": 2, "strict_interval": False})
        _, _, dc = self.raw("POST", "/common/oauth2/v2.0/devicecode", {"client_id": "cid", "scope": "offline_access"}, form=True)
        poll = {"grant_type": GRANT, "client_id": "cid", "device_code": dc["device_code"]}
        codes = [self.raw("POST", "/common/oauth2/v2.0/token", poll, form=True)[0] for _ in range(3)]
        self.assertEqual(codes, [400, 400, 200])

    def test_refresh_grant_rotation_and_revoke(self) -> None:
        tok = self.sign_in()
        form = {"grant_type": "refresh_token", "client_id": "cid", "refresh_token": tok["refresh_token"]}
        st, _, new = self.raw("POST", "/common/oauth2/v2.0/token", form, form=True)
        self.assertEqual(st, 200)
        self.assertNotEqual(new["refresh_token"], tok["refresh_token"])
        st, _, err = self.raw("POST", "/common/oauth2/v2.0/token", form, form=True)  # old one is dead
        self.assertEqual((st, err["error"]), (400, "invalid_grant"))
        self.ctl("revoke_refresh")
        form["refresh_token"] = new["refresh_token"]
        st, _, err = self.raw("POST", "/common/oauth2/v2.0/token", form, form=True)
        self.assertEqual((st, err["error"]), (400, "invalid_grant"))

    def test_graph_requires_valid_bearer(self) -> None:
        for headers in ({}, {"Authorization": "Bearer nonsense"}):
            st, _, body = self.raw("GET", "/v1.0/me", headers=headers)
            self.assertEqual(st, 401)
            self.assertEqual(body["error"]["code"], "InvalidAuthenticationToken")
        self.assertEqual(self.ctl("state")["counters"]["graph_401"], 2)

    def test_access_token_expiry_and_refresh(self) -> None:
        self.ctl("config", {"token_ttl": 1})
        tok = self.sign_in()
        self.assertEqual(self.g("GET", "/v1.0/me")[0], 200)
        time.sleep(1.2)
        st, _, body = self.g("GET", "/v1.0/me")
        self.assertEqual((st, body["error"]["code"]), (401, "InvalidAuthenticationToken"))
        st, _, new = self.raw("POST", "/common/oauth2/v2.0/token", {
            "grant_type": "refresh_token", "client_id": "cid", "refresh_token": tok["refresh_token"]}, form=True)
        self.assertEqual(st, 200)
        self.token = new["access_token"]
        self.assertEqual(self.g("GET", "/v1.0/me")[0], 200)
        self.assertEqual(self.ctl("state")["counters"]["refreshes"], 1)

    def test_expire_tokens_control(self) -> None:
        self.sign_in()
        self.ctl("expire_tokens")
        self.assertEqual(self.g("GET", "/v1.0/me")[0], 401)


class GraphTests(Base):
    def setUp(self) -> None:
        super().setUp()
        self.sign_in()

    def test_me_and_folders(self) -> None:
        self.load(5)
        st, h, me = self.g("GET", "/v1.0/me")
        self.assertEqual(st, 200)
        self.assertIn("@odata.context", me)
        self.assertTrue(h["content-type"].startswith("application/json"))
        st, _, inbox = self.g("GET", "/v1.0/me/mailFolders/inbox")
        self.assertEqual((st, inbox["totalItemCount"], inbox["displayName"]), (200, 5, "Inbox"))
        self.assertIn("@odata.context", inbox)
        st, _, by_id = self.g("GET", "/v1.0/me/mailFolders/" + inbox["id"])
        self.assertEqual(by_id["id"], inbox["id"])
        self.assertEqual(self.g("GET", "/v1.0/me/mailFolders/archive")[0], 200)
        self.assertEqual(self.g("GET", "/v1.0/me/mailFolders/bogus")[0], 404)

    def test_delta_full_enumeration_page_size_3_then_incremental_removed(self) -> None:
        ids = self.load(10)
        url = self.base + "/v1.0/me/mailFolders/inbox/messages/delta?$select=id,subject,isRead"
        items, pages, delta = self.delta_all(url, {"Prefer": 'odata.maxpagesize=3, IdType="ImmutableId"'})
        self.assertEqual([len(p) for p in pages], [3, 3, 3, 1])
        self.assertEqual(sorted(i["id"] for i in items), sorted(ids))
        self.assertEqual(len({i["id"] for i in items}), 10)
        self.assertNotIn("bodyPreview", items[0])  # $select projection
        self.assertIn("subject", items[0])
        self.assertIn("$deltatoken", delta)
        # another client moves a message away
        victim = ids[4]
        self.ctl("external", {"op": "move", "id": victim, "to": "archive"})
        items2, pages2, delta2 = self.delta_all(delta, IMM)
        self.assertEqual(items2, [{"@odata.type": "#microsoft.graph.message", "id": victim,
                                   "@removed": {"reason": "deleted"}}])
        items3, _, _ = self.delta_all(delta2, IMM)
        self.assertEqual(items3, [])
        # the archive folder sees it as an addition
        arch, _, _ = self.delta_all(self.base + "/v1.0/me/mailFolders/archive/messages/delta?$select=id", IMM)
        self.assertEqual([a["id"] for a in arch], [victim])

    def test_delta_incremental_add_delete_flag_partial_update(self) -> None:
        ids = self.load(6)
        _, _, delta = self.delta_all(self.base + "/v1.0/me/mailFolders/inbox/messages/delta",
                                     {"Prefer": "odata.maxpagesize=100, " + IMM["Prefer"]})
        self.ctl("external", {"op": "flag", "id": ids[0]})
        self.ctl("external", {"op": "delete", "id": ids[1]})
        added = self.ctl("external", {"op": "add", "count": 2, "seed": 5})["ids"]
        items, _, delta2 = self.delta_all(delta, IMM)
        by_id = {i["id"]: i for i in items}
        self.assertEqual(set(by_id), {ids[0], ids[1], *added})
        self.assertEqual(by_id[ids[1]]["@removed"]["reason"], "deleted")
        partial = by_id[ids[0]]
        self.assertEqual(set(partial), {"@odata.type", "@odata.etag", "id", "changeKey", "flag"})  # partial update
        self.assertEqual(partial["flag"]["flagStatus"], "flagged")
        for a in added:
            self.assertIn("subject", by_id[a])  # added messages are full
            self.assertIn("body", by_id[a])
        # switching partial updates off emits the full projection for updates
        self.ctl("config", {"delta_partial_updates": False})
        self.ctl("external", {"op": "unflag", "id": ids[0]})
        items, _, _ = self.delta_all(delta2, IMM)
        self.assertIn("subject", items[0])
        self.assertEqual(items[0]["flag"]["flagStatus"], "notFlagged")

    def test_delta_replay_duplicates_last_item(self) -> None:
        ids = self.load(7)
        self.ctl("config", {"delta_replay": True})
        items, pages, _ = self.delta_all(self.base + "/v1.0/me/mailFolders/inbox/messages/delta?$select=id",
                                         {"Prefer": 'odata.maxpagesize=3, IdType="ImmutableId"'})
        self.assertEqual(pages[1][0]["id"], pages[0][-1]["id"])
        self.assertEqual(set(i["id"] for i in items), set(ids))
        self.assertGreater(len(items), 7)

    def test_delta_trailing_empty_page(self) -> None:
        self.load(4)
        self.ctl("config", {"trailing_empty_page": True})
        _, pages, _ = self.delta_all(self.base + "/v1.0/me/mailFolders/inbox/messages/delta?$select=id",
                                     {"Prefer": "odata.maxpagesize=4"})
        self.assertEqual([len(p) for p in pages], [4, 0])

    def test_delta_default_page_size_is_10(self) -> None:
        self.load(25)
        _, pages, _ = self.delta_all(self.base + "/v1.0/me/mailFolders/inbox/messages/delta?$select=id")
        self.assertEqual([len(p) for p in pages], [10, 10, 5])

    def test_delta_410_variants(self) -> None:
        ids = self.load(4)
        _, _, delta = self.delta_all(self.base + "/v1.0/me/mailFolders/inbox/messages/delta?$select=id")
        st, _, body = self.g("GET", "/v1.0/me/mailFolders/inbox/messages/delta?$deltatoken=garbage")
        self.assertEqual((st, body["error"]["code"]), (410, "SyncStateNotFound"))
        self.ctl("expire_delta")  # wrong generation
        st, h, body = self.g("GET", delta)
        self.assertEqual((st, body["error"]["code"]), (410, "SyncStateNotFound"))
        self.assertIn("location", h)
        _, _, delta = self.delta_all(self.base + "/v1.0/me/mailFolders/inbox/messages/delta?$select=id")
        self.ctl("config", {"delta_expiry_changes": 2})  # token older than N changes
        for i in ids[:3]:
            self.ctl("external", {"op": "flag", "id": i})
        self.assertEqual(self.g("GET", delta)[0], 410)
        self.ctl("config", {"delta_expiry_status": 400})  # selectable 400 variant
        st, _, body = self.g("GET", delta)
        self.assertEqual((st, body["error"]["code"]), (400, "SyncStateNotFound"))

    def test_delta_filter_and_unsupported(self) -> None:
        self.load(10)
        self.assertEqual(self.g("GET", "/v1.0/me/mailFolders/inbox/messages/delta?$search=foo")[0], 400)
        self.assertEqual(self.g("GET", "/v1.0/me/mailFolders/inbox/messages/delta?$filter=isRead%20eq%20true")[0], 400)
        newest = max(m["receivedDateTime"] for m in generate_fixture.generate(10, 1)["messages"])
        items, _, _ = self.delta_all(self.base + "/v1.0/me/mailFolders/inbox/messages/delta?$select=id&$filter="
                                     + urllib.parse.quote("receivedDateTime ge " + newest))
        self.assertEqual(len(items), 1)

    def test_immutable_vs_mutable_move(self) -> None:
        ids = self.load(4)
        st, _, moved = self.g("POST", f"/v1.0/me/messages/{ids[0]}/move", {"destinationId": "archive"}, IMM)
        self.assertEqual(st, 201)  # Created, not 200
        self.assertEqual(moved["id"], ids[0])  # immutable id is stable across the move
        self.assertIn("@odata.context", moved)
        _, _, arch = self.g("GET", "/v1.0/me/mailFolders/archive")
        self.assertEqual(moved["parentFolderId"], arch["id"])
        self.assertEqual(self.g("GET", f"/v1.0/me/messages/{ids[0]}", headers=IMM)[0], 200)
        # no Prefer header: the move returns a NEW id and the old id then 404s
        mut_id = self.ctl("state")["messages"][ids[1]]["mutableId"]
        st, _, moved2 = self.g("POST", f"/v1.0/me/messages/{mut_id}/move", {"destinationId": arch["id"]})
        self.assertEqual(st, 201)
        self.assertNotEqual(moved2["id"], mut_id)
        st, _, body = self.g("GET", f"/v1.0/me/messages/{mut_id}")
        self.assertEqual((st, body["error"]["code"]), (404, "ErrorItemNotFound"))
        self.assertEqual(self.g("GET", f"/v1.0/me/messages/{moved2['id']}")[0], 200)
        self.assertEqual(self.g("GET", f"/v1.0/me/messages/{ids[1]}")[0], 404)  # immutable id needs the header
        state = self.ctl("state")
        self.assertEqual(state["messages"][ids[1]]["parentFolder"], "archive")
        self.assertEqual(state["messages"][ids[1]]["mutableId"], moved2["id"])

    def test_mutable_delta_removed_uses_old_id(self) -> None:
        ids = self.load(3)
        items, _, delta = self.delta_all(self.base + "/v1.0/me/mailFolders/inbox/messages/delta?$select=id")
        mut_id = self.ctl("state")["messages"][ids[2]]["mutableId"]
        self.assertIn(mut_id, {i["id"] for i in items})
        self.g("POST", f"/v1.0/me/messages/{mut_id}/move", {"destinationId": "deleteditems"})
        items2, _, _ = self.delta_all(delta)
        self.assertEqual([i["id"] for i in items2], [mut_id])
        self.assertIn("@removed", items2[0])

    def test_immutable_gap_mode(self) -> None:
        ids = self.load(2)
        _, _, delta = self.delta_all(self.base + "/v1.0/me/mailFolders/inbox/messages/delta?$select=id", IMM)
        self.ctl("config", {"immutable_gap": True})
        self.ctl("external", {"op": "move", "id": ids[0], "to": "archive"})
        self.ctl("external", {"op": "move", "id": ids[0], "to": "inbox"})
        items, _, _ = self.delta_all(delta, IMM)  # moved out and back in: only the @removed shows
        self.assertEqual([("@removed" in i, i["id"]) for i in items], [(True, ids[0])])

    def test_get_message_body_text_conversion(self) -> None:
        self.ctl("load", {"path": os.path.join(FIXTURE_DIR, "inbox_small.json")})
        mid = "small-02-html-script-tracker"
        st, h, msg = self.g("GET", f"/v1.0/me/messages/{mid}?$select=id,body,bodyPreview", None,
                            {"Prefer": 'outlook.body-content-type="text", IdType="ImmutableId"'})
        self.assertEqual(st, 200)
        self.assertEqual(msg["body"]["contentType"], "text")
        self.assertNotIn("<", msg["body"]["content"])
        self.assertNotIn("alert", msg["body"]["content"])
        self.assertIn("Big news", msg["body"]["content"])
        self.assertIn('outlook.body-content-type="text"', h["preference-applied"])
        self.assertIn("@odata.context", msg)
        self.assertLessEqual(len(msg["bodyPreview"]), 255)
        _, _, html_msg = self.g("GET", f"/v1.0/me/messages/{mid}", headers=IMM)
        self.assertEqual(html_msg["body"]["contentType"], "html")
        self.assertIn("<script>", html_msg["body"]["content"])
        # separate Prefer header lines are accepted too
        conn = http.client.HTTPConnection("127.0.0.1", self.srv.server_address[1], timeout=10)
        conn.putrequest("GET", f"/v1.0/me/messages/{mid}?$select=body")
        conn.putheader("Authorization", "Bearer " + str(self.token))
        conn.putheader("Prefer", 'IdType="ImmutableId"')
        conn.putheader("Prefer", 'outlook.body-content-type="text"')
        conn.endheaders()
        self.assertEqual(json.loads(conn.getresponse().read())["body"]["contentType"], "text")
        conn.close()

    def test_patch_flag_and_read(self) -> None:
        key = self.load(3)[0]
        path = f"/v1.0/me/messages/{key}"
        before = self.g("GET", path, headers=IMM)[2]["changeKey"]
        st, _, msg = self.g("PATCH", path, {"flag": {"flagStatus": "flagged"}}, IMM)
        self.assertEqual(st, 200)
        self.assertEqual(msg["flag"]["flagStatus"], "flagged")
        self.assertNotEqual(msg["changeKey"], before)
        self.assertIn("@odata.context", msg)
        st, _, msg = self.g("PATCH", path, {"isRead": True}, IMM)
        self.assertTrue(msg["isRead"])
        _, _, same = self.g("PATCH", path, {"flag": {"flagStatus": "flagged"}}, IMM)
        self.assertEqual(same["changeKey"], msg["changeKey"])  # idempotent: no spurious change
        self.assertEqual(self.ctl("state")["messages"][key]["flagStatus"], "flagged")
        self.assertEqual(self.g("PATCH", path, {"flag": {"flagStatus": "bogus"}}, IMM)[0], 400)
        self.assertEqual(self.g("PATCH", path, {"subject": "x"}, IMM)[0], 400)
        due = {"flag": {"flagStatus": "flagged", "dueDateTime": {"dateTime": "2026-10-21T00:00:00", "timeZone": "UTC"}}}
        self.assertEqual(self.g("PATCH", path, due, IMM)[0], 400)
        self.assertEqual(self.g("PATCH", path, {"isRead": False}, dict(IMM, **{"If-Match": 'W/"stale"'}))[0], 412)

    def test_unknown_id_404(self) -> None:
        for method, path, body in (("GET", "/v1.0/me/messages/nope", None),
                                   ("PATCH", "/v1.0/me/messages/nope", {"isRead": True}),
                                   ("POST", "/v1.0/me/messages/nope/move", {"destinationId": "archive"}),
                                   ("GET", "/v1.0/me/messages/nope/attachments", None)):
            st, _, err = self.g(method, path, body, IMM)
            self.assertEqual((st, err["error"]["code"]), (404, "ErrorItemNotFound"), path)

    def test_move_to_missing_archive_folder(self) -> None:
        ids = self.load(2)
        self.ctl("folders", {"archive": False})
        st, _, err = self.g("POST", f"/v1.0/me/messages/{ids[0]}/move", {"destinationId": "archive"}, IMM)
        self.assertEqual((st, err["error"]["code"]), (404, "ErrorFolderNotFound"))
        self.assertEqual(self.g("GET", "/v1.0/me/mailFolders/archive")[0], 404)
        self.assertEqual(self.ctl("state")["messages"][ids[0]]["parentFolder"], "inbox")
        self.assertEqual(self.g("POST", f"/v1.0/me/messages/{ids[0]}/move", {"destinationId": "deleteditems"}, IMM)[0], 201)
        self.ctl("folders", {"archive": True})
        self.assertEqual(self.g("POST", f"/v1.0/me/messages/{ids[1]}/move", {"destinationId": "archive"}, IMM)[0], 201)

    def test_move_requires_destination(self) -> None:
        ids = self.load(1)
        self.assertEqual(self.g("POST", f"/v1.0/me/messages/{ids[0]}/move", {}, IMM)[0], 400)

    def test_scope_check_requires_mail_readwrite_for_writes(self) -> None:
        ids = self.load(3)
        self.sign_in(scope="offline_access User.Read Mail.Read")
        path = f"/v1.0/me/messages/{ids[0]}"
        self.assertEqual(self.g("GET", path, headers=IMM)[0], 200)  # reads are fine
        for method, p, body in (("PATCH", path, {"isRead": True}),
                                ("POST", path + "/move", {"destinationId": "archive"}),
                                ("DELETE", path, None)):
            st, _, err = self.g(method, p, body, IMM)
            self.assertEqual((st, err["error"]["code"]), (403, "ErrorAccessDenied"), method)
        # a forbidden write must not have changed anything
        st = self.ctl("state")
        self.assertEqual(st["messages"][ids[0]]["parentFolder"], "inbox")
        self.assertFalse(st["messages"][ids[0]]["isRead"])
        # the same writes succeed with Mail.ReadWrite (any case, or the resource-qualified form)
        self.sign_in(scope="offline_access https://graph.microsoft.com/mail.readwrite")
        self.assertEqual(self.g("PATCH", path, {"isRead": True}, IMM)[0], 200)
        self.assertEqual(self.g("POST", path + "/move", {"destinationId": "archive"}, IMM)[0], 201)

    def test_scope_check_can_be_disabled(self) -> None:
        ids = self.load(1)
        self.sign_in(scope="offline_access User.Read")
        path = f"/v1.0/me/messages/{ids[0]}"
        self.assertEqual(self.g("PATCH", path, {"isRead": True}, IMM)[0], 403)
        self.ctl("config", {"scope_check": False})
        self.assertEqual(self.g("PATCH", path, {"isRead": True}, IMM)[0], 200)
        self.ctl("reset")  # reset restores the startup default (on)
        self.assertTrue(self.ctl("config")["scope_check"])

    def test_remove_already_moved_404_switch(self) -> None:
        ids = self.load(2)
        self.sign_in()
        move = lambda i: self.g("POST", f"/v1.0/me/messages/{i}/move", {"destinationId": "archive"}, IMM)  # noqa: E731
        self.assertEqual(move(ids[0])[0], 201)
        self.assertEqual(move(ids[0])[0], 201)  # default: re-move is a harmless no-op
        self.ctl("config", {"remove_already_moved_404": True})
        st, _, err = move(ids[0])
        self.assertEqual((st, err["error"]["code"]), (404, "ErrorItemNotFound"))
        self.assertEqual(self.ctl("state")["messages"][ids[0]]["parentFolder"], "archive")
        self.assertEqual(move(ids[1])[0], 201)  # a first move still works in this mode
        self.assertEqual(self.g("POST", f"/v1.0/me/messages/{ids[1]}/move", {"destinationId": "deleteditems"}, IMM)[0], 201)

    def test_delete_endpoint_is_soft_and_counted(self) -> None:
        ids = self.load(2)
        self.assertEqual(self.g("DELETE", f"/v1.0/me/messages/{ids[0]}", None, IMM)[0], 204)
        state = self.ctl("state")
        self.assertEqual(state["messages"][ids[0]]["parentFolder"], "deleteditems")
        self.assertEqual(state["counters"]["delete_calls"], 1)


class AttachmentTests(Base):
    def setUp(self) -> None:
        super().setUp()
        self.sign_in()
        self.ctl("load", {"path": os.path.join(FIXTURE_DIR, "inbox_small.json")})

    def test_three_attachment_types_and_select(self) -> None:
        st, _, body = self.g("GET", "/v1.0/me/messages/small-08-inline-item-reference/attachments"
                             "?$select=id,name,contentType,size,isInline", None, IMM)
        self.assertEqual(st, 200)
        self.assertIn("@odata.context", body)
        types = [a["@odata.type"] for a in body["value"]]
        self.assertEqual(types, ["#microsoft.graph.fileAttachment", "#microsoft.graph.itemAttachment",
                                 "#microsoft.graph.referenceAttachment"])
        self.assertTrue(body["value"][0]["isInline"])
        for a in body["value"]:
            self.assertNotIn("contentBytes", a)
            self.assertEqual(set(a) - {"@odata.type"}, {"id", "name", "contentType", "size", "isInline"})
        self.assertEqual(self.ctl("state")["counters"]["content_bytes_served"], 0)

    def test_without_select_content_bytes_served_and_counted(self) -> None:
        st, _, body = self.g("GET", "/v1.0/me/messages/small-07-pdf-docx-png/attachments", None, IMM)
        self.assertEqual(st, 200)
        self.assertEqual(len(body["value"]), 3)
        self.assertTrue(all("contentBytes" in a for a in body["value"]))
        self.assertEqual(self.ctl("state")["counters"]["content_bytes_served"], 3)

    def test_doc_style_odata_type(self) -> None:
        self.ctl("config", {"doc_style_odata_type": True})
        _, _, body = self.g("GET", "/v1.0/me/messages/small-07-pdf-docx-png/attachments?$select=name", None, IMM)
        self.assertEqual(body["value"][0]["@odata.type"], "microsoft.graph.fileAttachment")

    def test_extreme_metadata_passthrough(self) -> None:
        _, _, body = self.g("GET", "/v1.0/me/messages/small-09-attachment-extremes/attachments?$select=name,size", None, IMM)
        self.assertEqual(len(body["value"][0]["name"]), 10004)
        self.assertEqual(body["value"][1]["size"], -1)

    def test_has_attachments_ignores_inline_only(self) -> None:
        _, _, a = self.g("GET", "/v1.0/me/messages/small-11-inline-only?$select=hasAttachments", None, IMM)
        _, _, b = self.g("GET", "/v1.0/me/messages/small-07-pdf-docx-png?$select=hasAttachments", None, IMM)
        self.assertEqual((a["hasAttachments"], b["hasAttachments"]), (False, True))
        _, _, atts = self.g("GET", "/v1.0/me/messages/small-11-inline-only/attachments?$select=id", None, IMM)
        self.assertEqual(len(atts["value"]), 1)  # still listed

    def test_value_fetch_forbidden_and_counted(self) -> None:
        for path in ("/v1.0/me/messages/small-07-pdf-docx-png/attachments/att-000007-0/$value",
                     "/v1.0/me/messages/small-07-pdf-docx-png/$value"):
            st, _, err = self.g("GET", path, None, IMM)
            self.assertEqual((st, err["error"]["code"]), (403, "ErrorAccessDenied"))
        counters = self.ctl("state")["counters"]
        self.assertEqual(counters["forbidden_fetch"], 2)
        self.assertEqual(counters["by_path"]["GET /v1.0/me/messages/{id}/attachments/{id}/$value"], 1)


class FaultTests(Base):
    def setUp(self) -> None:
        super().setUp()
        self.sign_in()

    def test_429_retry_after_consumed_once(self) -> None:
        self.ctl("fault", {"match": "/v1.0/me", "method": "GET", "status": 429, "retry_after": 2, "count": 1})
        st, h, body = self.g("GET", "/v1.0/me")
        self.assertEqual((st, h["retry-after"], body["error"]["code"]), (429, "2", "TooManyRequests"))
        self.assertEqual(self.g("GET", "/v1.0/me")[0], 200)

    def test_429_without_retry_after_header(self) -> None:
        self.ctl("fault", {"match": "/v1.0/me", "status": 429, "retry_after": None})
        st, h, _ = self.g("GET", "/v1.0/me")
        self.assertEqual(st, 429)
        self.assertNotIn("retry-after", h)

    def test_fault_default_retry_after_custom_body_and_count(self) -> None:
        self.ctl("fault", {"match": "mailFolders", "status": 503, "count": 2, "body": {"error": {"code": "X"}}})
        for _ in range(2):
            st, h, body = self.g("GET", "/v1.0/me/mailFolders/inbox")
            self.assertEqual((st, body), (503, {"error": {"code": "X"}}))
            self.assertEqual(h["retry-after"], "1")
        self.assertEqual(self.g("GET", "/v1.0/me/mailFolders/inbox")[0], 200)

    def test_fault_on_oauth_endpoint(self) -> None:
        self.ctl("fault", {"match": "oauth2/v2.0/token", "status": 503, "count": 1})
        st, _, _ = self.raw("POST", "/common/oauth2/v2.0/token", {"grant_type": "refresh_token"}, form=True)
        self.assertEqual(st, 503)

    def test_drop_closes_connection(self) -> None:
        self.ctl("drop", {"match": "/v1.0/me/mailFolders/inbox", "count": 1})
        with self.assertRaises(NET_ERRORS):
            self.g("GET", "/v1.0/me/mailFolders/inbox")
        self.assertEqual(self.g("GET", "/v1.0/me/mailFolders/inbox")[0], 200)
        log = [r for r in self.ctl("requests") if r["path"].endswith("mailFolders/inbox")]
        self.assertEqual([r["status"] for r in log], [0, 200])

    def test_drop_mid_delta_page_two(self) -> None:
        self.ctl("load", {"generate": 9, "seed": 3})
        url = self.base + "/v1.0/me/mailFolders/inbox/messages/delta?$select=id"
        _, _, page1 = self.g("GET", url, None, {"Prefer": "odata.maxpagesize=3"})
        self.ctl("drop", {"match": "$skiptoken"})
        with self.assertRaises(NET_ERRORS):
            self.g("GET", page1["@odata.nextLink"])
        self.assertEqual(self.g("GET", page1["@odata.nextLink"])[0], 200)  # same link resumes

    def test_drop_after_executes_then_drops(self) -> None:
        self.ctl("load", {"generate": 2, "seed": 1})
        ids = list(self.ctl("state")["folders"]["inbox"])
        self.ctl("drop", {"match": "/move", "after": True})
        with self.assertRaises(NET_ERRORS):
            self.g("POST", f"/v1.0/me/messages/{ids[0]}/move", {"destinationId": "archive"}, IMM)
        self.assertEqual(self.ctl("state")["messages"][ids[0]]["parentFolder"], "archive")
        # the retry is idempotent with immutable ids
        self.assertEqual(self.g("POST", f"/v1.0/me/messages/{ids[0]}/move", {"destinationId": "archive"}, IMM)[0], 201)
        self.assertEqual(len(self.ctl("state")["folders"]["archive"]), 1)

    def test_clear_faults(self) -> None:
        self.ctl("fault", {"match": "/v1.0/me", "status": 500, "count": -1})
        self.assertEqual(self.g("GET", "/v1.0/me")[0], 500)
        self.assertEqual(self.g("GET", "/v1.0/me")[0], 500)
        self.ctl("clear_faults")
        self.assertEqual(self.g("GET", "/v1.0/me")[0], 200)


class ControlTests(Base):
    def test_state_requests_and_counters(self) -> None:
        self.load(3)
        self.sign_in()
        self.g("GET", "/v1.0/me/mailFolders/inbox/messages/delta?$select=id",
               None, {"Prefer": 'odata.maxpagesize=2, IdType="ImmutableId"'})
        st = self.ctl("state")
        self.assertEqual(set(st["folders"]), {"inbox", "archive", "deleteditems"})
        any_id = st["folders"]["inbox"][0]
        self.assertTrue(set(st["messages"][any_id]) >= {"subject", "flagStatus", "parentFolder"})
        self.assertGreaterEqual(st["counters"]["requests"], 3)
        self.assertEqual(st["counters"]["forbidden_fetch"], 0)
        self.assertIn("GET /v1.0/me/mailFolders/inbox/messages/delta", st["counters"]["by_path"])
        reqs = self.ctl("requests")
        last = reqs[-1]
        self.assertEqual(last["method"], "GET")
        self.assertIn("IdType", last["headers"]["Prefer"])
        self.assertNotIn("fake-at", json.dumps(reqs))  # tokens never recorded
        self.assertEqual(last["body_length"], 0)

    def test_request_log_holds_more_than_200_and_is_capped_at_5000(self) -> None:
        self.sign_in()
        self.ctl("clear_log")
        for _ in range(250):
            self.g("GET", "/v1.0/me")
        self.assertEqual(len(self.ctl("requests")), 250)
        self.assertEqual(self.srv.app.log.maxlen, 5000)

    def test_request_invariant_counters_do_not_depend_on_the_log(self) -> None:
        self.sign_in()
        ids = self.load(1)
        self.ctl("clear_log")
        self.g("GET", "/v1.0/me", headers=IMM)
        self.g("GET", f"/v1.0/me/messages/{ids[0]}/attachments?$select=id,contentBytes", headers=IMM)
        self.g("GET", f"/v1.0/me/messages/{ids[0]}/$value", headers=IMM)
        self.g("GET", "/v1.0/me")  # no Prefer: IdType
        self.g("GET", "/v1.0/me", headers={"Prefer": 'odata.maxpagesize=5'})
        for _ in range(300):  # push the early requests out of any short log
            self.g("GET", "/v1.0/me", headers=IMM)
        c = self.ctl("state")["counters"]
        self.assertEqual(c["requests_missing_immutable_prefer"], 2)
        self.assertEqual(c["requests_with_value_or_contentbytes"], 2)
        self.assertEqual(c["graph_requests"], 305)
        # oauth and control calls are not Graph requests
        self.sign_in()
        self.assertEqual(self.ctl("state")["counters"]["graph_requests"], 305)
        self.ctl("clear_log")
        c = self.ctl("state")["counters"]
        self.assertEqual((c["requests_missing_immutable_prefer"], c["requests_with_value_or_contentbytes"]), (0, 0))

    def test_reset_and_load_fixtures(self) -> None:
        self.assertEqual(self.ctl("load", {"path": os.path.join(FIXTURE_DIR, "inbox_small.json")})["loaded"], 12)
        self.assertEqual(len(self.ctl("state")["folders"]["inbox"]), 12)
        self.ctl("reset")
        self.assertEqual(self.ctl("state")["folders"]["inbox"], [])
        self.assertEqual(self.raw("POST", "/__control/load", {"path": "/nonexistent.json"})[0], 400)
        self.assertEqual(self.raw("POST", "/__control/bogus", {})[0], 404)

    def test_external_ops(self) -> None:
        ids = self.load(4)
        self.ctl("external", {"op": "read", "id": ids[0]})
        self.ctl("external", {"op": "flag", "id": ids[1]})
        self.ctl("external", {"op": "delete", "id": ids[2]})
        self.ctl("external", {"op": "delete", "id": ids[3], "hard": True})
        added = self.ctl("external", {"op": "add", "message": {"subject": "hello",
                                                               "body": {"contentType": "text", "content": "x"}}})["ids"]
        st = self.ctl("state")
        self.assertTrue(st["messages"][ids[0]]["isRead"])
        self.assertEqual(st["messages"][ids[1]]["flagStatus"], "flagged")
        self.assertEqual(st["messages"][ids[2]]["parentFolder"], "deleteditems")
        self.assertNotIn(ids[3], st["messages"])
        self.assertEqual(st["messages"][added[0]]["subject"], "hello")
        self.assertEqual(self.raw("POST", "/__control/external", {"op": "move", "id": "nope", "to": "inbox"})[0], 404)

    def test_load_append_keeps_existing(self) -> None:
        self.load(3, 1)
        self.ctl("load", {"generate": 2, "seed": 2, "append": True})
        self.assertEqual(len(self.ctl("state")["folders"]["inbox"]), 5)

    def test_reset_restores_startup_config(self) -> None:
        self.ctl("config", {"token_ttl": 1})
        self.ctl("reset")
        self.assertEqual(self.ctl("config")["token_ttl"], 3600.0)

    def test_clear_log_keeps_mailbox(self) -> None:
        self.load(2)
        self.sign_in()
        self.g("GET", "/v1.0/me")
        self.ctl("clear_log")
        st = self.ctl("state")
        self.assertEqual(st["counters"]["requests"], 0)
        self.assertEqual(len(st["folders"]["inbox"]), 2)
        self.assertEqual(self.ctl("requests"), [])


class FixtureTests(unittest.TestCase):
    def test_generator_deterministic_and_shaped(self) -> None:
        a = generate_fixture.generate(1000, 42)
        self.assertEqual(json.dumps(a, sort_keys=True), json.dumps(generate_fixture.generate(1000, 42), sort_keys=True))
        self.assertNotEqual(json.dumps(a, sort_keys=True), json.dumps(generate_fixture.generate(1000, 43), sort_keys=True))
        msgs = a["messages"]
        self.assertEqual(len(msgs), 1000)
        self.assertEqual(len({m["id"] for m in msgs}), 1000)
        self.assertEqual(sum(m["flag"]["flagStatus"] == "flagged" for m in msgs), 100)
        with_att = [m for m in msgs if "attachments" in m]
        self.assertEqual(len(with_att), 200)
        self.assertTrue(any(len(x["name"]) >= 10000 for m in with_att for x in m["attachments"]))
        self.assertTrue(any(x["size"] == -1 for m in with_att for x in m["attachments"]))
        self.assertEqual(len({x["@odata.type"] for m in with_att for x in m["attachments"]}), 3)
        self.assertTrue(any(x.get("isInline") for m in with_att for x in m["attachments"]))
        self.assertTrue(any(len(m["subject"]) > 400 for m in msgs))
        self.assertTrue(any(m["subject"] == "" for m in msgs))
        self.assertTrue(any("address" not in m.get("from", {"emailAddress": {}})["emailAddress"] for m in msgs))
        self.assertTrue(any("<script" in m["body"]["content"] for m in msgs))
        self.assertTrue(any("track.example.test" in m["body"]["content"] for m in msgs))
        self.assertEqual({m["body"]["contentType"] for m in msgs}, {"html", "text"})
        stamps = sorted(generate_fixture._epoch(m["receivedDateTime"]) for m in msgs)
        self.assertGreater(stamps[-1] - stamps[0], 80 * 86400)
        self.assertLessEqual(stamps[-1] - stamps[0], 90 * 86400)

    def test_cli_writes_loadable_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "f.json")
            self.assertEqual(generate_fixture.main(["--count", "30", "--seed", "5", "--out", out]), 0)
            with open(out, encoding="utf-8") as fh:
                self.assertEqual(len(json.load(fh)["messages"]), 30)
            self.assertEqual(fake.App(fake.Config()).load_file(out), 30)

    def test_committed_fixtures_load(self) -> None:
        app = fake.App(fake.Config())
        self.assertEqual(app.load_file(os.path.join(FIXTURE_DIR, "inbox_small.json")), 12)
        self.assertEqual(app.load_file(os.path.join(FIXTURE_DIR, "inbox_1000.json")), 1000)


class SubprocessTests(unittest.TestCase):
    server_py = os.path.join(HERE, "server.py")

    def test_port_zero_prints_listening_first(self) -> None:
        proc = subprocess.Popen([sys.executable, "-I", self.server_py, "--port", "0", "--generate", "5",
                                 "--exit-on-stdin-eof"],
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        try:
            line = proc.stdout.readline()
            self.assertTrue(line.startswith("LISTENING "), line)
            port = int(line.split()[1])
            self.assertGreater(port, 0)
            with urllib.request.urlopen(f"http://127.0.0.1:{port}/__control/state", timeout=5) as r:
                self.assertEqual(len(json.load(r)["folders"]["inbox"]), 5)
            proc.stdin.close()  # parent "died": the server must exit
            self.assertEqual(proc.wait(timeout=5), 0)
        finally:
            if proc.poll() is None:
                proc.kill()
            proc.stdout.close()

    def test_verbose_logs_requests_without_tokens(self) -> None:
        proc = subprocess.Popen([sys.executable, "-I", self.server_py, "--port", "0", "--verbose"],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            port = int(proc.stdout.readline().split()[1])
            req = urllib.request.Request(f"http://127.0.0.1:{port}/v1.0/me",
                                         headers={"Authorization": "Bearer secret-token-xyz"})
            try:
                urllib.request.urlopen(req, timeout=5)
            except urllib.error.HTTPError:
                pass
            proc.terminate()
            err = proc.stderr.read()
            self.assertIn("GET /v1.0/me -> 401", err)
            self.assertNotIn("secret-token-xyz", err)
        finally:
            proc.kill()
            proc.stdout.close()
            proc.stderr.close()

    def test_bad_fixture_exits_nonzero_without_listening(self) -> None:
        r = subprocess.run([sys.executable, "-I", self.server_py, "--port", "0", "--load", "/nope.json"],
                           capture_output=True, text=True, timeout=10)
        self.assertEqual(r.returncode, 2)
        self.assertEqual(r.stdout, "")

    def test_delta_partial_updates_cli_flag(self) -> None:
        import server as s
        self.assertTrue(s.parse_args([]).delta_partial_updates)
        self.assertFalse(s.parse_args(["--no-delta-partial-updates"]).delta_partial_updates)
        self.assertTrue(s.parse_args(["--delta-replay"]).delta_replay)

    def test_scope_check_and_already_moved_cli_flags(self) -> None:
        import server as s
        self.assertTrue(s.parse_args([]).scope_check)
        self.assertFalse(s.parse_args(["--no-scope-check"]).scope_check)
        self.assertFalse(s.parse_args([]).remove_already_moved_404)
        self.assertTrue(s.parse_args(["--remove-already-moved-404"]).remove_already_moved_404)


if __name__ == "__main__":
    unittest.main(verbosity=1)
