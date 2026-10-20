#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "fake_transport.hpp"
#include "slowmail/core/clock.hpp"
#include "slowmail/graph/graph_backend.hpp"

using namespace slowmail;
using namespace slowmail::graph;
using slowmail::graph::testing::FakeTransport;
using nlohmann::json;

namespace {

// Token provider that counts invalidations and rotates its token on each one.
class CountingTokens final : public TokenProvider {
public:
    core::Outcome<std::string> access_token() override { return "tok-" + std::to_string(generation_); }
    void invalidate() override {
        ++invalidations;
        ++generation_;
    }
    int invalidations = 0;
private:
    int generation_ = 0;
};

constexpr const char* kBase = "https://graph.microsoft.com/v1.0";

struct Fixture : ::testing::Test {
    std::shared_ptr<FakeTransport> http = std::make_shared<FakeTransport>();
    std::shared_ptr<CountingTokens> tokens = std::make_shared<CountingTokens>();
    std::shared_ptr<core::FakeClock> clock = std::make_shared<core::FakeClock>();
    GraphConfig cfg;

    Fixture() { cfg.sleep_on_retry = false; }
    GraphBackend backend() { return GraphBackend(cfg, http, tokens, clock); }

    static std::string full_message(const std::string& id, const std::string& subject = "Hello") {
        json j = {{"id", id},
                  {"conversationId", "conv-" + id},
                  {"subject", subject},
                  {"from", {{"emailAddress", {{"name", "Dana Swope"}, {"address", "dana@contoso.example"}}}}},
                  {"receivedDateTime", "2026-10-20T01:23:45Z"},
                  {"isRead", false},
                  {"flag", {{"flagStatus", "flagged"}}},
                  {"bodyPreview", "preview text"},
                  {"hasAttachments", true},
                  {"changeKey", "ck-" + id},
                  {"parentFolderId", "inbox-folder"}};
        return j.dump();
    }
    static std::string delta_page(const std::vector<std::string>& items, const std::string& link_key,
                                  const std::string& link) {
        std::string v = "[";
        for (std::size_t i = 0; i < items.size(); ++i) v += (i ? "," : "") + items[i];
        v += "]";
        return "{\"value\":" + v + ",\"" + link_key + "\":\"" + link + "\"}";
    }
    static std::string header_of(const HttpRequest& r, const std::string& k) {
        auto it = r.headers.find(k);
        return it == r.headers.end() ? std::string() : it->second;
    }
};

} // namespace

// --------------------------------------------------------------------------- parsing
TEST_F(Fixture, ParseDeltaPageWithNextLinkAndRemoved) {
    const std::string body = delta_page(
        {full_message("m1"),
         R"({"@odata.type":"#microsoft.graph.message","id":"m2","@removed":{"reason":"deleted"}})",
         R"({"id":"m3","@removed":{"reason":"changed"}})"},
        "@odata.nextLink", "https://graph.microsoft.com/v1.0/next?$skiptoken=abc");
    auto r = GraphBackend::parse_delta_page(body);
    ASSERT_TRUE(r.ok());
    const auto& p = r.value();
    ASSERT_EQ(p.changed.size(), 1u);
    EXPECT_EQ(p.changed[0].id, "m1");
    EXPECT_EQ(p.changed[0].subject, "Hello");
    EXPECT_EQ(p.changed[0].from.address, "dana@contoso.example");
    EXPECT_EQ(p.changed[0].from.name, "Dana Swope");
    EXPECT_EQ(p.changed[0].received_at_iso, "2026-10-20T01:23:45Z");
    EXPECT_EQ(p.changed[0].received_at_epoch, 1792459425);
    EXPECT_EQ(p.changed[0].flag_status, core::FlagStatus::Flagged);
    EXPECT_TRUE(p.changed[0].has_attachments);
    EXPECT_EQ(p.changed[0].change_key, "ck-m1");
    EXPECT_EQ(p.changed[0].parent_folder_id, "inbox-folder");
    ASSERT_EQ(p.removed_ids.size(), 2u);
    EXPECT_EQ(p.removed_ids[0], "m2");
    EXPECT_EQ(p.removed_ids[1], "m3");
    ASSERT_TRUE(p.next_link.has_value());
    EXPECT_EQ(*p.next_link, "https://graph.microsoft.com/v1.0/next?$skiptoken=abc");
    EXPECT_FALSE(p.delta_link.has_value());
}

TEST_F(Fixture, ParseDeltaPageWithDeltaLinkAndEmptyValue) {
    auto r = GraphBackend::parse_delta_page(R"({"value":[],"@odata.deltaLink":"https://x/delta?$deltatoken=t"})");
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().changed.empty());
    ASSERT_TRUE(r.value().delta_link.has_value());
    EXPECT_EQ(*r.value().delta_link, "https://x/delta?$deltatoken=t");
    EXPECT_FALSE(r.value().next_link.has_value());
}

TEST_F(Fixture, ParseMessageSummaryToleratesMissingFieldsAndSanitises) {
    auto r = GraphBackend::parse_message_summary(json::parse(R"({"id":"only-id"})"));
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().id, "only-id");
    EXPECT_EQ(r.value().subject, "");
    EXPECT_EQ(r.value().received_at_epoch, 0);

    json j = {{"id", "x"}, {"subject", std::string("Hi\x01 there\nnow")}, {"isRead", "true"}, {"hasAttachments", "false"},
              {"bodyPreview", std::string(1000, 'a')}};
    auto s = GraphBackend::parse_message_summary(j);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(s.value().subject.find('\x01'), std::string::npos);
    EXPECT_EQ(s.value().subject.find('\n'), std::string::npos);
    EXPECT_TRUE(s.value().is_read);  // string "true" accepted
    EXPECT_FALSE(s.value().has_attachments);
    EXPECT_LE(s.value().body_preview.size(), core::Limits::kMaxPreview);

    EXPECT_FALSE(GraphBackend::parse_message_summary(json::parse(R"({"subject":"no id"})")).ok());
}

TEST_F(Fixture, MalformedJsonIsMalformed) {
    auto r = GraphBackend::parse_delta_page("{not json");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().kind, core::ErrorKind::Malformed);

    http->expect("GET", "/delta", 200, "<html>oops</html>");
    auto b = backend();
    auto d = b.fetch_inbox_delta("");
    ASSERT_FALSE(d.ok());
    EXPECT_EQ(d.error().kind, core::ErrorKind::Malformed);
}

// --------------------------------------------------------------------------- delta requests
TEST_F(Fixture, InitialDeltaRequestHasSelectAndPreferHeaders) {
    http->expect("GET", "/delta", 200, delta_page({}, "@odata.deltaLink", "https://graph.microsoft.com/v1.0/d?$deltatoken=1"));
    auto b = backend();
    auto r = b.fetch_inbox_delta("");
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(http->requests().size(), 1u);
    const auto& req = http->requests()[0];
    EXPECT_EQ(req.method, "GET");
    EXPECT_EQ(req.url.rfind(std::string(kBase) + "/me/mailFolders/inbox/messages/delta?", 0), 0u);
    EXPECT_NE(req.url.find("$select=id,conversationId,subject,from,receivedDateTime,isRead,flag,bodyPreview,hasAttachments,changeKey,parentFolderId"),
              std::string::npos);
    const std::string prefer = header_of(req, "Prefer");
    EXPECT_NE(prefer.find("odata.maxpagesize=50"), std::string::npos);
    EXPECT_NE(prefer.find("IdType=\"ImmutableId\""), std::string::npos);
    EXPECT_EQ(header_of(req, "Authorization"), "Bearer tok-0");
    EXPECT_EQ(header_of(req, "Accept"), "application/json");
}

TEST_F(Fixture, NextLinkIsUsedVerbatim) {
    const std::string link = "https://graph.microsoft.com/v1.0/me/mailfolders('AQMk')/messages/delta?$skiptoken=GwcB";
    http->expect("GET", "skiptoken", 200, delta_page({full_message("a")}, "@odata.deltaLink", "https://graph.microsoft.com/v1.0/d?$deltatoken=2"));
    auto b = backend();
    auto r = b.fetch_inbox_delta(link);
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(http->requests().size(), 1u);
    EXPECT_EQ(http->requests()[0].url, link);
    EXPECT_EQ(http->requests()[0].url.find("$select"), std::string::npos);
    EXPECT_NE(header_of(http->requests()[0], "Prefer").find("IdType=\"ImmutableId\""), std::string::npos);
}

TEST_F(Fixture, LinkToForeignOriginIsRefusedWithoutSendingToken) {
    auto b = backend();
    auto r = b.fetch_inbox_delta("https://evil.example/v1.0/me/delta?$skiptoken=x");
    ASSERT_FALSE(r.ok());
    // Gone, not Invalid: a stored link from another endpoint (data dir reused with a different base URL) makes the
    // core do a full resync instead of failing forever. The bearer token is still never sent there.
    EXPECT_EQ(r.error().kind, core::ErrorKind::Gone);
    EXPECT_TRUE(http->requests().empty());
    auto r2 = b.fetch_inbox_delta("https://graph.microsoft.com@evil.example/v1.0/me/delta");
    ASSERT_FALSE(r2.ok());
    EXPECT_EQ(r2.error().kind, core::ErrorKind::Gone);
    EXPECT_TRUE(http->requests().empty());
}

TEST_F(Fixture, StoredLinkFromDifferentBaseUrlIsGoneThenFullResyncWorks) {
    auto b = backend();
    const std::string stale = "http://127.0.0.1:8765/v1.0/me/mailFolders('a')/messages/delta?$deltatoken=old";
    auto gone = b.fetch_inbox_delta(stale);
    ASSERT_FALSE(gone.ok());
    EXPECT_EQ(gone.error().kind, core::ErrorKind::Gone);
    EXPECT_TRUE(http->requests().empty());
    // the core then asks for a full resync with an empty link, which goes to the configured endpoint
    http->expect("GET", "/delta", 200, delta_page({full_message("m1")}, "@odata.deltaLink", std::string(kBase) + "/d?$deltatoken=1"));
    auto full = b.fetch_inbox_delta("");
    ASSERT_TRUE(full.ok());
    ASSERT_EQ(http->requests().size(), 1u);
    EXPECT_EQ(http->requests()[0].url.rfind(kBase, 0), 0u);
}

TEST_F(Fixture, ImmutableIdPreferIsSentOnEveryRequestKind) {
    http->expect("GET", "/delta", 200, delta_page({R"({"id":"p1","isRead":true,"changeKey":"k"})"}, "@odata.nextLink", "https://graph.microsoft.com/v1.0/n?$skiptoken=1"));
    http->expect("GET", "/me/messages/p1", 200, full_message("p1"));
    http->expect("GET", "/me/messages/b1?", 200, R"({"id":"b1","hasAttachments":true,"body":{"contentType":"text","content":"hi"}})");
    http->expect("GET", "/attachments", 200, R"({"value":[]})");
    http->expect("POST", "/move", 201, R"({"id":"b1","parentFolderId":"arch"})");
    http->expect("PATCH", "/me/messages/b1", 200, "{}");
    http->expect("GET", "/me?", 200, R"({"id":"u","mail":"a@b.example"})");
    auto b = backend();
    ASSERT_TRUE(b.fetch_inbox_delta("").ok());
    ASSERT_TRUE(b.fetch_body("b1").ok());
    ASSERT_TRUE(b.move("b1", core::WellKnownFolder::Archive).ok());
    ASSERT_TRUE(b.set_flag("b1", core::FlagStatus::Flagged).ok());
    ASSERT_TRUE(b.account().ok());
    EXPECT_EQ(http->requests().size(), 7u);
    EXPECT_EQ(http->unmatched(), 0u);
    for (const auto& r : http->requests()) {
        EXPECT_NE(header_of(r, "Prefer").find("IdType=\"ImmutableId\""), std::string::npos) << r.method << " " << r.url;
    }
}

TEST_F(Fixture, ImmutableIdPreferCanBeDisabled) {
    cfg.prefer_immutable_ids = false;
    http->expect("PATCH", "/me/messages/x", 200, "{}");
    auto b = backend();
    ASSERT_TRUE(b.set_flag("x", core::FlagStatus::Flagged).ok());
    EXPECT_EQ(header_of(http->requests()[0], "Prefer"), "");
}

// --------------------------------------------------------------------------- partial delta items
TEST_F(Fixture, PartialDeltaItemIsReplacedByFullFetch) {
    http->expect("GET", "/delta", 200,
                 delta_page({full_message("full1", "Complete"), R"({"id":"part/1+","isRead":true,"changeKey":"ck2"})"},
                            "@odata.deltaLink", "https://graph.microsoft.com/v1.0/d?$deltatoken=9"));
    http->expect("GET", "/me/messages/part%2F1%2B?", 200, full_message("part/1+", "Fetched in full"));
    auto b = backend();
    auto r = b.fetch_inbox_delta("");
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value().changed.size(), 2u);
    EXPECT_EQ(r.value().changed[0].subject, "Complete");
    EXPECT_EQ(r.value().changed[1].id, "part/1+");
    EXPECT_EQ(r.value().changed[1].subject, "Fetched in full");
    EXPECT_EQ(r.value().changed[1].from.address, "dana@contoso.example");
    ASSERT_EQ(http->requests().size(), 2u);
    const std::string& u = http->requests()[1].url;
    EXPECT_NE(u.find("$select=id,conversationId,subject,from,receivedDateTime,isRead,flag,bodyPreview,hasAttachments,changeKey,parentFolderId"), std::string::npos);
    EXPECT_NE(header_of(http->requests()[1], "Prefer").find("IdType=\"ImmutableId\""), std::string::npos);
    EXPECT_TRUE(r.value().removed_ids.empty());
}

TEST_F(Fixture, PartialDeltaItemThatVanishedBecomesRemoved) {
    http->expect("GET", "/delta", 200,
                 delta_page({full_message("keep"), R"({"id":"gone1","isRead":true})"}, "@odata.deltaLink",
                            "https://graph.microsoft.com/v1.0/d?$deltatoken=9"));
    http->expect("GET", "/me/messages/gone1", 404, R"({"error":{"code":"ErrorItemNotFound","message":"x"}})");
    auto b = backend();
    auto r = b.fetch_inbox_delta("");
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value().changed.size(), 1u);
    EXPECT_EQ(r.value().changed[0].id, "keep");
    ASSERT_EQ(r.value().removed_ids.size(), 1u);
    EXPECT_EQ(r.value().removed_ids[0], "gone1");
}

TEST_F(Fixture, PartialFollowUpNon404ItemNotFoundIsAnErrorNotARemoval) {
    // A proxy page, captive portal or wrong base URL answers 404 without a Graph error code. That must not strip
    // items from the page and still complete the sync.
    const char* kBodies[] = {"<html><body>404 Not Found</body></html>", "", "{}", R"({"error":{"message":"x"}})",
                             R"({"error":{"code":"ErrorFolderNotFound","message":"x"}})",
                             R"({"error":{"code":"ResourceNotFound","message":"x"}})"};
    for (const char* body : kBodies) {
        http = std::make_shared<FakeTransport>();
        http->expect("GET", "/delta", 200,
                     delta_page({full_message("keep"), R"({"id":"p1","isRead":true})", R"({"id":"p2","isRead":true})"},
                                "@odata.deltaLink", "https://graph.microsoft.com/v1.0/d?$deltatoken=9"));
        http->expect("GET", "/me/messages/p1", 404, body);
        auto b = backend();
        auto r = b.fetch_inbox_delta("");
        ASSERT_FALSE(r.ok()) << "body: " << body;
        EXPECT_EQ(r.error().kind, core::ErrorKind::NotFound) << "body: " << body;
        EXPECT_EQ(r.error().http_status, 404);
        EXPECT_NE(r.error().code, "ErrorItemNotFound");
        EXPECT_EQ(http->requests().size(), 2u);  // stopped at the first failing follow-up
    }
}

TEST_F(Fixture, PartialFollowUpOtherErrorsPropagate) {
    http->expect("GET", "/delta", 200, delta_page({R"({"id":"p","isRead":true})"}, "@odata.deltaLink", "https://graph.microsoft.com/v1.0/d?$deltatoken=9"));
    http->expect("GET", "/me/messages/p", 500, "{}");
    auto b = backend();
    auto r = b.fetch_inbox_delta("");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().kind, core::ErrorKind::Server);
}

// --------------------------------------------------------------------------- 401 handling
TEST_F(Fixture, Unauthorized401InvalidatesAndRetriesOnce) {
    http->expect("GET", "/delta", 401, R"({"error":{"code":"InvalidAuthenticationToken","message":"expired"}})");
    http->expect("GET", "/delta", 200, delta_page({}, "@odata.deltaLink", "https://graph.microsoft.com/v1.0/d?$deltatoken=1"));
    auto b = backend();
    ASSERT_TRUE(b.fetch_inbox_delta("").ok());
    EXPECT_EQ(tokens->invalidations, 1);
    ASSERT_EQ(http->requests().size(), 2u);
    EXPECT_EQ(header_of(http->requests()[0], "Authorization"), "Bearer tok-0");
    EXPECT_EQ(header_of(http->requests()[1], "Authorization"), "Bearer tok-1");
}

TEST_F(Fixture, SecondUnauthorizedIsAuthError) {
    http->expect("GET", "/delta", 401, R"({"error":{"code":"InvalidAuthenticationToken","message":"expired"}})");
    http->expect("GET", "/delta", 401, R"({"error":{"code":"InvalidAuthenticationToken","message":"expired"}})");
    auto b = backend();
    auto r = b.fetch_inbox_delta("");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().kind, core::ErrorKind::Auth);
    EXPECT_EQ(r.error().code, "InvalidAuthenticationToken");
    EXPECT_EQ(http->requests().size(), 2u);
    EXPECT_EQ(tokens->invalidations, 1);
}

TEST_F(Fixture, TokenProviderAuthFailureIsReturned) {
    class Failing final : public TokenProvider {
    public:
        core::Outcome<std::string> access_token() override {
            core::Error e;
            e.kind = core::ErrorKind::Auth;
            e.message = "sign-in required";
            return e;
        }
        void invalidate() override {}
    };
    GraphBackend b(cfg, http, std::make_shared<Failing>(), clock);
    auto r = b.fetch_inbox_delta("");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().kind, core::ErrorKind::Auth);
    EXPECT_TRUE(http->requests().empty());
    auto a = b.account();  // unauthenticated -> signed_in=false, no error
    ASSERT_TRUE(a.ok());
    EXPECT_FALSE(a.value().signed_in);
}

// --------------------------------------------------------------------------- throttling
TEST_F(Fixture, Throttle429WithRetryAfterIsRetried) {
    http->expect("GET", "/delta", 429, R"({"error":{"code":"TooManyRequests","message":"later"}})", {{"retry-after", "1"}});
    http->expect("GET", "/delta", 200, delta_page({}, "@odata.deltaLink", "https://graph.microsoft.com/v1.0/d?$deltatoken=1"));
    auto b = backend();
    ASSERT_TRUE(b.fetch_inbox_delta("").ok());
    EXPECT_EQ(http->requests().size(), 2u);
    EXPECT_EQ(tokens->invalidations, 0);
}

TEST_F(Fixture, Throttle503WithRetryAfterIsRetried) {
    http->expect("PATCH", "/me/messages/x", 503, "", {{"retry-after", "2"}});
    http->expect("PATCH", "/me/messages/x", 200, "{}");
    auto b = backend();
    EXPECT_TRUE(b.set_flag("x", core::FlagStatus::NotFlagged).ok());
    EXPECT_EQ(http->requests().size(), 2u);
}

TEST_F(Fixture, LongRetryAfterIsReturnedAsThrottled) {
    http->expect("GET", "/delta", 429, R"({"error":{"code":"TooManyRequests","message":"later"}})", {{"retry-after", "999"}});
    auto b = backend();
    auto r = b.fetch_inbox_delta("");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().kind, core::ErrorKind::Throttled);
    ASSERT_TRUE(r.error().retry_after_seconds.has_value());
    EXPECT_EQ(*r.error().retry_after_seconds, 999);
    EXPECT_EQ(r.error().code, "TooManyRequests");
    EXPECT_EQ(http->requests().size(), 1u);
}

TEST_F(Fixture, Throttle429WithoutRetryAfterBacksOffThenReportsNoRetryAfter) {
    for (int i = 0; i < 4; ++i) http->expect("GET", "/delta", 429, R"({"error":{"code":"TooManyRequests","message":"x"}})");
    auto b = backend();
    auto r = b.fetch_inbox_delta("");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().kind, core::ErrorKind::Throttled);
    EXPECT_FALSE(r.error().retry_after_seconds.has_value());
    EXPECT_EQ(http->requests().size(), 4u);  // 1 + max_retries_on_throttle(3)
}

TEST_F(Fixture, ThrottleRetriesAreBoundedWithRetryAfter) {
    cfg.max_retries_on_throttle = 2;
    for (int i = 0; i < 3; ++i) http->expect("GET", "/delta", 429, "{}", {{"retry-after", "3"}});
    auto b = backend();
    auto r = b.fetch_inbox_delta("");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().kind, core::ErrorKind::Throttled);
    EXPECT_EQ(*r.error().retry_after_seconds, 3);
    EXPECT_EQ(http->requests().size(), 3u);
}

TEST_F(Fixture, Html503WithoutRetryAfterIsServerError) {
    for (int i = 0; i < 4; ++i) http->expect("GET", "/delta", 503, "<html>Service Unavailable</html>");
    auto b = backend();
    auto r = b.fetch_inbox_delta("");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().kind, core::ErrorKind::Server);
    EXPECT_EQ(r.error().http_status, 503);
}

// --------------------------------------------------------------------------- error mapping
TEST_F(Fixture, MapHttpErrorTable) {
    auto map = [](int status, const std::string& body, std::map<std::string, std::string> h = {}) {
        HttpResponse r;
        r.status = status;
        r.body = body;
        r.headers = std::move(h);
        return GraphBackend::map_http_error(r);
    };
    EXPECT_EQ(map(401, "{}").kind, core::ErrorKind::Auth);
    EXPECT_EQ(map(403, R"({"error":{"code":"ErrorAccessDenied"}})").kind, core::ErrorKind::Forbidden);
    EXPECT_EQ(map(404, R"({"error":{"code":"ErrorItemNotFound","message":"m"}})").kind, core::ErrorKind::NotFound);
    EXPECT_EQ(map(404, R"({"error":{"code":"ErrorItemNotFound","message":"m"}})").code, "ErrorItemNotFound");
    EXPECT_EQ(map(409, "{}").kind, core::ErrorKind::Conflict);
    EXPECT_EQ(map(412, "{}").kind, core::ErrorKind::Conflict);
    EXPECT_EQ(map(410, "{}").kind, core::ErrorKind::Gone);
    EXPECT_EQ(map(500, "{}").kind, core::ErrorKind::Server);
    EXPECT_EQ(map(502, "").kind, core::ErrorKind::Server);
    EXPECT_EQ(map(503, "").kind, core::ErrorKind::Server);
    EXPECT_EQ(map(503, "", {{"retry-after", "7"}}).kind, core::ErrorKind::Throttled);
    EXPECT_EQ(map(429, "", {{"retry-after", "7"}}).retry_after_seconds.value_or(-1), 7);
    EXPECT_EQ(map(400, "{}").kind, core::ErrorKind::Invalid);
    EXPECT_EQ(map(404, R"({"error":{"message":"m","innererror":{"code":"inner"}}})").code, "inner");  // lower-case variant
    EXPECT_EQ(map(404, "{broken").code, "");
    // Non-JSON bodies (proxy / captive portal / wrong base URL): empty code, and the message says why.
    for (const char* body : {"<html>Not Found</html>", "", "{broken", "plain text"}) {
        const auto e = map(404, body);
        EXPECT_EQ(e.kind, core::ErrorKind::NotFound);
        EXPECT_EQ(e.code, "");
        EXPECT_EQ(e.message, "non-JSON 404 response");
        EXPECT_EQ(e.http_status, 404);
    }
    EXPECT_EQ(map(502, "<html>Bad gateway</html>").message, "non-JSON 502 response");
    EXPECT_EQ(map(502, "<html>Bad gateway</html>").kind, core::ErrorKind::Server);
    EXPECT_EQ(map(404, "{}").message.find("non-JSON"), std::string::npos);  // JSON without an error object is not "non-JSON"
    EXPECT_EQ(map(404, R"({"error":{"code":"ErrorItemNotFound","message":"secret subject"}})").message.find("secret"),
              std::string::npos);  // server message text is never copied into Error::message
}

TEST_F(Fixture, NotFound404CarriesGraphCode) {
    http->expect("PATCH", "/me/messages/x", 404, R"({"error":{"code":"ErrorItemNotFound","message":"The specified object was not found in the store."}})");
    auto b = backend();
    auto s = b.set_flag("x", core::FlagStatus::Flagged);
    ASSERT_FALSE(s.ok());
    EXPECT_EQ(s.error().kind, core::ErrorKind::NotFound);
    EXPECT_EQ(s.error().code, "ErrorItemNotFound");
    EXPECT_EQ(s.error().http_status, 404);
}

TEST_F(Fixture, MoveFolderNotFoundIsDistinguishableFromItemNotFound) {
    http->expect("POST", "/move", 404, R"({"error":{"code":"ErrorFolderNotFound","message":"The specified folder could not be found in the store."}})");
    auto b = backend();
    auto r = b.move("m1", core::WellKnownFolder::Archive);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().kind, core::ErrorKind::NotFound);
    EXPECT_EQ(r.error().code, "ErrorFolderNotFound");
}

TEST_F(Fixture, Delta410IsGone) {
    http->expect("GET", "skiptoken", 410, R"({"error":{"code":"syncStateNotFound","message":"x"}})");
    auto b = backend();
    auto r = b.fetch_inbox_delta("https://graph.microsoft.com/v1.0/me/mailfolders('a')/messages/delta?$skiptoken=zz");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().kind, core::ErrorKind::Gone);
}

TEST_F(Fixture, DeltaSyncStateCodeOn4xxIsGone) {
    const std::string link = "https://graph.microsoft.com/v1.0/me/mailfolders('a')/messages/delta?$deltatoken=old";
    http->expect("GET", "deltatoken", 400, R"({"error":{"code":"SyncStateNotFound","message":"x"}})");
    http->expect("GET", "deltatoken", 400, R"({"error":{"code":"ErrorInvalidSyncStateData","message":"x"}})");
    http->expect("GET", "deltatoken", 400, R"({"error":{"code":"BadRequest","message":"x"}})");
    http->expect("GET", "deltatoken", 404, R"({"error":{"code":"SyncStateNotFound","message":"x"}})");
    auto b = backend();
    auto r1 = b.fetch_inbox_delta(link);
    ASSERT_FALSE(r1.ok());
    EXPECT_EQ(r1.error().kind, core::ErrorKind::Gone);
    EXPECT_EQ(r1.error().code, "SyncStateNotFound");
    EXPECT_EQ(b.fetch_inbox_delta(link).error().kind, core::ErrorKind::Gone);
    EXPECT_EQ(b.fetch_inbox_delta(link).error().kind, core::ErrorKind::Invalid);   // unrelated 400 stays a plain error
    EXPECT_EQ(b.fetch_inbox_delta(link).error().kind, core::ErrorKind::NotFound);  // 404 is excluded from the Gone rule
}

TEST_F(Fixture, TransportErrorPropagates) {
    core::Error e;
    e.kind = core::ErrorKind::Network;
    e.message = "timeout";
    http->expect_error("GET", "/delta", e);
    auto b = backend();
    auto r = b.fetch_inbox_delta("");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().kind, core::ErrorKind::Network);
}

// --------------------------------------------------------------------------- move / flag / account
TEST_F(Fixture, MoveSendsDestinationIdAndParsesResult) {
    http->expect("POST", "/me/messages/AAMk%2Fx%3D/move", 201, R"({"id":"NEWID","parentFolderId":"arch-id","subject":"ignored"})");
    auto b = backend();
    auto r = b.move("AAMk/x=", core::WellKnownFolder::Archive);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().new_id, "NEWID");
    EXPECT_EQ(r.value().new_folder_id, "arch-id");
    const auto& req = http->requests()[0];
    EXPECT_EQ(req.method, "POST");
    EXPECT_EQ(json::parse(req.body), json::parse(R"({"destinationId":"archive"})"));
    EXPECT_EQ(header_of(req, "Content-Type"), "application/json");
}

TEST_F(Fixture, MoveToDeletedItemsUsesWellKnownName) {
    http->expect("POST", "/move", 201, R"({"id":"m","parentFolderId":"del"})");
    auto b = backend();
    ASSERT_TRUE(b.move("m", core::WellKnownFolder::DeletedItems).ok());
    EXPECT_EQ(json::parse(http->requests()[0].body)["destinationId"], "deleteditems");
}

TEST_F(Fixture, MoveWithoutIdInResponseIsMalformed) {
    http->expect("POST", "/move", 201, R"({"parentFolderId":"x"})");
    auto b = backend();
    auto r = b.move("m", core::WellKnownFolder::Archive);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().kind, core::ErrorKind::Malformed);
}

TEST_F(Fixture, SetFlagSendsPatchBody) {
    http->expect("PATCH", "/me/messages/m1", 200, "{}");
    http->expect("PATCH", "/me/messages/m1", 200, "{}");
    http->expect("PATCH", "/me/messages/m1", 200, "{}");
    auto b = backend();
    ASSERT_TRUE(b.set_flag("m1", core::FlagStatus::Flagged).ok());
    ASSERT_TRUE(b.set_flag("m1", core::FlagStatus::NotFlagged).ok());
    ASSERT_TRUE(b.set_flag("m1", core::FlagStatus::Complete).ok());
    const auto& rs = http->requests();
    EXPECT_EQ(rs[0].method, "PATCH");
    EXPECT_EQ(json::parse(rs[0].body), json::parse(R"({"flag":{"flagStatus":"flagged"}})"));
    EXPECT_EQ(json::parse(rs[1].body), json::parse(R"({"flag":{"flagStatus":"notFlagged"}})"));
    EXPECT_EQ(header_of(rs[0], "Content-Type"), "application/json");
    const json complete = json::parse(rs[2].body);
    EXPECT_EQ(complete["flag"]["flagStatus"], "complete");
    EXPECT_EQ(complete["flag"]["completedDateTime"]["timeZone"], "UTC");
}

TEST_F(Fixture, SetFlagValidatesFlagStatusInResponse) {
    auto b = backend();
    // matching body (case-insensitive compare of the enum string), non-JSON, and JSON without a flag object: accepted
    http->expect("PATCH", "/me/messages/m1", 200, full_message("m1"));  // full_message has flagStatus "flagged"
    EXPECT_TRUE(b.set_flag("m1", core::FlagStatus::Flagged).ok());
    http->expect("PATCH", "/me/messages/m1", 200, R"({"flag":{"flagStatus":"notFlagged"}})");
    EXPECT_TRUE(b.set_flag("m1", core::FlagStatus::NotFlagged).ok());
    http->expect("PATCH", "/me/messages/m1", 200, R"({"flag":{"flagStatus":"complete","completedDateTime":{}}})");
    EXPECT_TRUE(b.set_flag("m1", core::FlagStatus::Complete).ok());
    http->expect("PATCH", "/me/messages/m1", 200, "not json");
    EXPECT_TRUE(b.set_flag("m1", core::FlagStatus::Flagged).ok());
    http->expect("PATCH", "/me/messages/m1", 200, R"({"id":"m1"})");
    EXPECT_TRUE(b.set_flag("m1", core::FlagStatus::Flagged).ok());
    http->expect("PATCH", "/me/messages/m1", 204, "");
    EXPECT_TRUE(b.set_flag("m1", core::FlagStatus::Flagged).ok());

    // mismatching status: Malformed, not success
    http->expect("PATCH", "/me/messages/m1", 200, R"({"flag":{"flagStatus":"notFlagged"}})");
    auto s1 = b.set_flag("m1", core::FlagStatus::Flagged);
    ASSERT_FALSE(s1.ok());
    EXPECT_EQ(s1.error().kind, core::ErrorKind::Malformed);
    http->expect("PATCH", "/me/messages/m1", 200, full_message("m1"));  // says "flagged" but we asked for complete
    auto s2 = b.set_flag("m1", core::FlagStatus::Complete);
    ASSERT_FALSE(s2.ok());
    EXPECT_EQ(s2.error().kind, core::ErrorKind::Malformed);
    // a flag object without flagStatus is not a confirmation either
    http->expect("PATCH", "/me/messages/m1", 200, R"({"flag":{}})");
    EXPECT_EQ(b.set_flag("m1", core::FlagStatus::Flagged).error().kind, core::ErrorKind::Malformed);
    EXPECT_EQ(http->unmatched(), 0u);
}

TEST_F(Fixture, SetFlagFailureIsReported) {
    http->expect("PATCH", "/me/messages/m1", 409, R"({"error":{"code":"ErrorIrresolvableConflict"}})");
    auto b = backend();
    auto s = b.set_flag("m1", core::FlagStatus::Flagged);
    ASSERT_FALSE(s.ok());
    EXPECT_EQ(s.error().kind, core::ErrorKind::Conflict);
}

TEST_F(Fixture, AccountReadsMe) {
    http->expect("GET", "/me?", 200, R"({"id":"user-1","userPrincipalName":"upn@contoso.example","mail":null})");
    auto b = backend();
    auto a = b.account();
    ASSERT_TRUE(a.ok());
    EXPECT_TRUE(a.value().signed_in);
    EXPECT_EQ(a.value().user_id, "user-1");
    EXPECT_EQ(a.value().display_address, "upn@contoso.example");
    EXPECT_NE(http->requests()[0].url.find("$select=id,userPrincipalName,mail"), std::string::npos);
}

TEST_F(Fixture, AccountUnauthorizedMeansSignedOut) {
    http->expect("GET", "/me?", 401, "{}");
    http->expect("GET", "/me?", 401, "{}");
    auto b = backend();
    auto a = b.account();
    ASSERT_TRUE(a.ok());
    EXPECT_FALSE(a.value().signed_in);
}

// --------------------------------------------------------------------------- body + attachments
TEST_F(Fixture, FetchBodyConvertsHtmlAndClassifiesAttachments) {
    const std::string html =
        "<html><head><style>p{color:red}</style></head><body><p>Hello <b>world</b></p>"
        "<script>alert('x')</script><img src=\"https://tracker.example/p.gif\"></body></html>";
    json msg = {{"id", "m1"}, {"hasAttachments", true}, {"bodyPreview", "Hello world"},
                {"body", {{"contentType", "html"}, {"content", html}}}};
    http->expect("GET", "/me/messages/m1?", 200, msg.dump());
    json atts = {{"value",
                  json::array({{{"@odata.type", "#microsoft.graph.fileAttachment"}, {"id", "a1"}, {"name", "q3.pdf"},
                                {"contentType", "application/pdf"}, {"size", 183421}, {"isInline", false},
                                {"contentBytes", "SECRETBYTES"}},
                               {{"@odata.type", "#microsoft.graph.itemAttachment"}, {"id", "a2"}, {"name", "Re: budget"},
                                {"contentType", nullptr}, {"size", 2411}, {"isInline", false}},
                               {{"@odata.type", "microsoft.graph.referenceAttachment"}, {"id", "a3"}, {"name", "plan.docx"},
                                {"contentType", nullptr}, {"size", 745}, {"isInline", true}},
                               {{"@odata.type", "#microsoft.graph.somethingNew"}, {"id", "a4"}, {"name", "x"}}})}};
    http->expect("GET", "/attachments", 200, atts.dump());
    auto b = backend();
    auto r = b.fetch_body("m1");
    ASSERT_TRUE(r.ok());
    const auto& body = r.value();
    EXPECT_EQ(body.id, "m1");
    EXPECT_TRUE(body.was_html);
    EXPECT_FALSE(body.truncated);
    EXPECT_NE(body.text.find("Hello world"), std::string::npos);
    EXPECT_EQ(body.text.find("alert"), std::string::npos);
    EXPECT_EQ(body.text.find("color:red"), std::string::npos);
    EXPECT_EQ(body.text.find("tracker.example"), std::string::npos);
    EXPECT_EQ(body.text.find('<'), std::string::npos);

    ASSERT_EQ(body.attachments.size(), 4u);
    EXPECT_EQ(body.attachments[0].kind, core::AttachmentKind::File);
    EXPECT_EQ(body.attachments[0].name, "q3.pdf");
    EXPECT_EQ(body.attachments[0].content_type, "application/pdf");
    EXPECT_EQ(body.attachments[0].size, 183421);
    EXPECT_EQ(body.attachments[1].kind, core::AttachmentKind::Item);
    EXPECT_EQ(body.attachments[2].kind, core::AttachmentKind::Reference);
    EXPECT_TRUE(body.attachments[2].is_inline);
    EXPECT_EQ(body.attachments[3].kind, core::AttachmentKind::Unknown);

    ASSERT_EQ(http->requests().size(), 2u);
    EXPECT_NE(header_of(http->requests()[0], "Prefer").find("outlook.body-content-type=\"text\""), std::string::npos);
    EXPECT_NE(http->requests()[0].url.find("$select=id,body,bodyPreview,hasAttachments"), std::string::npos);
    EXPECT_NE(http->requests()[1].url.find("/attachments?$select=id,name,contentType,size,isInline"), std::string::npos);
    for (const auto& req : http->requests()) {
        EXPECT_EQ(req.url.find("$value"), std::string::npos);
        EXPECT_EQ(req.url.find("contentBytes"), std::string::npos);
        EXPECT_EQ(req.method, "GET");
    }
}

TEST_F(Fixture, FetchBodyPlainTextIsSanitisedAndNoAttachmentCallWhenNone) {
    json msg = {{"id", "m2"}, {"hasAttachments", false},
                {"body", {{"contentType", "text"}, {"content", std::string("line1\r\nline2\x01 end")}}}};
    http->expect("GET", "/me/messages/m2?", 200, msg.dump());
    auto b = backend();
    auto r = b.fetch_body("m2");
    ASSERT_TRUE(r.ok());
    EXPECT_FALSE(r.value().was_html);
    EXPECT_EQ(r.value().text.find('\x01'), std::string::npos);
    EXPECT_NE(r.value().text.find("line2"), std::string::npos);
    EXPECT_TRUE(r.value().attachments.empty());
    EXPECT_EQ(http->requests().size(), 1u);
}

TEST_F(Fixture, FetchBodyListsAttachmentsForInlineCidMarkerEvenIfFlagFalse) {
    json msg = {{"id", "m3"}, {"hasAttachments", false}, {"body", {{"contentType", "text"}, {"content", "see [cid:img1@x]"}}}};
    http->expect("GET", "/me/messages/m3?", 200, msg.dump());
    http->expect("GET", "/attachments", 200, R"({"value":[{"@odata.type":"#microsoft.graph.fileAttachment","id":"i","name":"img.png","contentType":"image/png","size":10,"isInline":true}]})");
    auto b = backend();
    auto r = b.fetch_body("m3");
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.value().attachments.size(), 1u);
    EXPECT_TRUE(r.value().attachments[0].is_inline);
}

TEST_F(Fixture, FetchBodyCapsHugeBody) {
    json msg = {{"id", "m4"}, {"hasAttachments", false},
                {"body", {{"contentType", "text"}, {"content", std::string(core::Limits::kMaxBodyChars + 5000, 'x')}}}};
    http->expect("GET", "/me/messages/m4?", 200, msg.dump());
    auto b = backend();
    auto r = b.fetch_body("m4");
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r.value().truncated);
    EXPECT_LE(r.value().text.size(), core::Limits::kMaxBodyChars);
}

TEST_F(Fixture, AttachmentListForbiddenOrMissingIsTolerated) {
    for (int status : {403, 404}) {
        http->expect("GET", "/me/messages/m5?", 200, R"({"id":"m5","hasAttachments":true,"body":{"contentType":"text","content":"hi"}})");
        http->expect("GET", "/attachments", status, R"({"error":{"code":"ErrorAccessDenied","message":"x"}})");
        auto b = backend();
        auto r = b.fetch_body("m5");
        ASSERT_TRUE(r.ok()) << status;
        EXPECT_TRUE(r.value().attachments.empty());
        EXPECT_EQ(r.value().text, "hi");
    }
}

TEST_F(Fixture, AttachmentListServerErrorPropagates) {
    http->expect("GET", "/me/messages/m6?", 200, R"({"id":"m6","hasAttachments":true,"body":{"contentType":"text","content":"hi"}})");
    http->expect("GET", "/attachments", 500, "{}");
    auto b = backend();
    auto r = b.fetch_body("m6");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().kind, core::ErrorKind::Server);
}

TEST_F(Fixture, FetchBodyNotFoundAndMalformed) {
    http->expect("GET", "/me/messages/gone?", 404, R"({"error":{"code":"ErrorItemNotFound","message":"x"}})");
    http->expect("GET", "/me/messages/bad?", 200, R"({"id":"bad"})");
    http->expect("GET", "/me/messages/junk?", 200, "<<<");
    auto b = backend();
    auto a = b.fetch_body("gone");
    ASSERT_FALSE(a.ok());
    EXPECT_EQ(a.error().kind, core::ErrorKind::NotFound);
    EXPECT_EQ(a.error().code, "ErrorItemNotFound");
    EXPECT_EQ(b.fetch_body("bad").error().kind, core::ErrorKind::Malformed);
    EXPECT_EQ(b.fetch_body("junk").error().kind, core::ErrorKind::Malformed);
    EXPECT_EQ(b.fetch_body("").error().kind, core::ErrorKind::Invalid);
}
