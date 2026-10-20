#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "fake_transport.hpp"
#include "slowmail/core/clock.hpp"
#include "slowmail/core/memory_store.hpp"
#include "slowmail/graph/auth.hpp"

using namespace slowmail;
using namespace slowmail::graph;
using slowmail::graph::testing::FakeTransport;
using nlohmann::json;

namespace {

std::string b64url(const std::string& in) {
    static const char* const t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    unsigned buf = 0;
    int bits = 0;
    for (unsigned char c : in) {
        buf = (buf << 8) | c;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out.push_back(t[(buf >> bits) & 0x3F]);
        }
    }
    if (bits > 0) out.push_back(t[(buf << (6 - bits)) & 0x3F]);
    return out;
}

struct AuthFixture : ::testing::Test {
    std::shared_ptr<FakeTransport> http = std::make_shared<FakeTransport>();
    core::MemoryStore store;
    std::shared_ptr<core::FakeClock> clock = std::make_shared<core::FakeClock>(1'800'000'000);
    AuthConfig cfg;

    AuthFixture() {
        cfg.client_id = "client-123";
        cfg.tenant = "contoso.example";
    }
    DeviceCodeAuth auth() { return DeviceCodeAuth(cfg, http, store, clock); }

    static std::string token_body(const std::string& at, const std::string& rt, int expires_in = 3599,
                                  const std::string& id_token = "") {
        json j = {{"token_type", "Bearer"}, {"scope", "Mail.ReadWrite User.Read"}, {"expires_in", expires_in},
                  {"access_token", at}};
        if (!rt.empty()) j["refresh_token"] = rt;
        if (!id_token.empty()) j["id_token"] = id_token;
        return j.dump();
    }
    static std::string err_body(const std::string& e, const std::string& desc = "") {
        return json{{"error", e}, {"error_description", desc}}.dump();
    }
    static DeviceCodeChallenge challenge() {
        DeviceCodeChallenge c;
        c.device_code = "DEVICE-SECRET";
        c.user_code = "ABCD1234";
        c.verification_uri = "https://microsoft.com/devicelogin";
        c.interval = 5;
        c.expires_in = 900;
        return c;
    }
    void store_tokens(std::int64_t expires_at, const std::string& at = "old-at", const std::string& rt = "old-rt") {
        TokenSet t;
        t.access_token = at;
        t.refresh_token = rt;
        t.expires_at = expires_at;
        t.scope = "Mail.ReadWrite";
        ASSERT_TRUE(store.put_token_blob(t.to_json()).ok());
    }
};

} // namespace

TEST(TokenSetTest, RoundTrip) {
    TokenSet t;
    t.access_token = "at";
    t.refresh_token = "rt";
    t.expires_at = 1'800'003'599;
    t.scope = "Mail.ReadWrite User.Read";
    t.account_hint = "user@contoso.example";
    auto back = TokenSet::from_json(t.to_json());
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->access_token, "at");
    EXPECT_EQ(back->refresh_token, "rt");
    EXPECT_EQ(back->expires_at, 1'800'003'599);
    EXPECT_EQ(back->scope, "Mail.ReadWrite User.Read");
    EXPECT_EQ(back->account_hint, "user@contoso.example");
}

TEST(TokenSetTest, RejectsGarbage) {
    EXPECT_FALSE(TokenSet::from_json("not json").has_value());
    EXPECT_FALSE(TokenSet::from_json("[1,2]").has_value());
    EXPECT_FALSE(TokenSet::from_json("").has_value());
    auto partial = TokenSet::from_json(R"({"refresh_token":"x","expires_at":"bad"})");
    ASSERT_TRUE(partial.has_value());
    EXPECT_EQ(partial->refresh_token, "x");
    EXPECT_EQ(partial->expires_at, 0);
}

TEST(PollResultTest, ToString) {
    EXPECT_STREQ(to_string(PollResult::SlowDown), "slow_down");
    EXPECT_STREQ(to_string(PollResult::Authorized), "authorized");
}

TEST_F(AuthFixture, BeginPostsFormAndParsesChallenge) {
    http->expect("POST", "/contoso.example/oauth2/v2.0/devicecode", 200,
                 R"({"device_code":"DEV","user_code":"ABCD1234","verification_uri":"https://microsoft.com/devicelogin","expires_in":900,"interval":5,"message":"To sign in, open the page and enter ABCD1234"})");
    auto a = auth();
    auto r = a.begin();
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().device_code, "DEV");
    EXPECT_EQ(r.value().user_code, "ABCD1234");
    EXPECT_EQ(r.value().verification_uri, "https://microsoft.com/devicelogin");
    EXPECT_EQ(r.value().expires_in, 900);
    EXPECT_EQ(r.value().interval, 5);
    EXPECT_FALSE(r.value().message.empty());
    const auto& req = http->requests()[0];
    EXPECT_EQ(req.url, "https://login.microsoftonline.com/contoso.example/oauth2/v2.0/devicecode");
    EXPECT_EQ(req.headers.at("Content-Type"), "application/x-www-form-urlencoded");
    EXPECT_EQ(req.body, "client_id=client-123&scope=offline_access+User.Read+Mail.ReadWrite");
}

TEST_F(AuthFixture, BeginRequiresClientIdAndValidResponse) {
    cfg.client_id.clear();
    auto none = auth();
    EXPECT_EQ(none.begin().error().kind, core::ErrorKind::Invalid);
    EXPECT_TRUE(http->requests().empty());

    cfg.client_id = "c";
    http->expect("POST", "devicecode", 200, R"({"user_code":"X"})");
    http->expect("POST", "devicecode", 200, "junk");
    http->expect("POST", "devicecode", 400, err_body("invalid_client", "AADSTS7000218: ..."));
    http->expect("POST", "devicecode", 400, err_body("access_denied", "AADSTS530003: blocked by Conditional Access"));
    auto a = auth();
    EXPECT_EQ(a.begin().error().kind, core::ErrorKind::Malformed);
    EXPECT_EQ(a.begin().error().kind, core::ErrorKind::Malformed);
    auto e = a.begin();
    EXPECT_EQ(e.error().kind, core::ErrorKind::Auth);
    EXPECT_EQ(e.error().code, "invalid_client");
    EXPECT_EQ(a.begin().error().kind, core::ErrorKind::Forbidden);  // device code blocked by Conditional Access
}

TEST_F(AuthFixture, PollStateMachine) {
    http->expect("POST", "/token", 400, err_body("authorization_pending"));
    http->expect("POST", "/token", 400, err_body("slow_down"));
    http->expect("POST", "/token", 400, err_body("authorization_declined"));
    http->expect("POST", "/token", 400, err_body("expired_token"));
    http->expect("POST", "/token", 400, err_body("bad_verification_code"));
    http->expect("POST", "/token", 400, err_body("something_else"));
    auto a = auth();
    const auto c = challenge();
    EXPECT_EQ(a.poll(c).value(), PollResult::Pending);
    EXPECT_EQ(a.poll(c).value(), PollResult::SlowDown);
    EXPECT_EQ(a.poll(c).value(), PollResult::Declined);
    EXPECT_EQ(a.poll(c).value(), PollResult::Expired);
    EXPECT_EQ(a.poll(c).value(), PollResult::BadCode);
    EXPECT_EQ(a.poll(c).value(), PollResult::Error);
    EXPECT_FALSE(store.get_token_blob().has_value());
    EXPECT_FALSE(a.signed_in());

    const auto& req = http->requests()[0];
    EXPECT_EQ(req.body, "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code&client_id=client-123&device_code=DEVICE-SECRET");
    EXPECT_EQ(req.url, "https://login.microsoftonline.com/contoso.example/oauth2/v2.0/token");
}

TEST_F(AuthFixture, PollServerAndNetworkFailuresAreErrors) {
    http->expect("POST", "/token", 503, "<html/>");
    core::Error ne;
    ne.kind = core::ErrorKind::Network;
    ne.message = "down";
    http->expect_error("POST", "/token", ne);
    auto a = auth();
    EXPECT_EQ(a.poll(challenge()).error().kind, core::ErrorKind::Server);
    EXPECT_EQ(a.poll(challenge()).error().kind, core::ErrorKind::Network);
}

TEST_F(AuthFixture, PollSuccessPersistsTokensAndReadsAccountHint) {
    const std::string payload = b64url(R"({"preferred_username":"dana@contoso.example","tid":"t"})");
    const std::string id_token = b64url(R"({"alg":"none"})") + "." + payload + ".sig";
    http->expect("POST", "/token", 200, token_body("AT1", "RT1", 3599, id_token));
    auto a = auth();
    auto r = a.poll(challenge());
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), PollResult::Authorized);
    auto blob = store.get_token_blob();
    ASSERT_TRUE(blob.has_value());
    auto ts = TokenSet::from_json(*blob);
    ASSERT_TRUE(ts.has_value());
    EXPECT_EQ(ts->access_token, "AT1");
    EXPECT_EQ(ts->refresh_token, "RT1");
    EXPECT_EQ(ts->expires_at, 1'800'000'000 + 3599);
    EXPECT_EQ(ts->account_hint, "dana@contoso.example");
    EXPECT_TRUE(a.signed_in());
    auto tok = a.access_token();  // served from cache, no extra request
    ASSERT_TRUE(tok.ok());
    EXPECT_EQ(tok.value(), "AT1");
    EXPECT_EQ(http->requests().size(), 1u);
}

TEST_F(AuthFixture, PollStorageFailureIsReported) {
    http->expect("POST", "/token", 200, token_body("AT1", "RT1"));
    store.fail_next_write();
    auto a = auth();
    auto r = a.poll(challenge());
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().kind, core::ErrorKind::Storage);
}

TEST_F(AuthFixture, AccessTokenWithoutSignInIsAuthError) {
    auto a = auth();
    auto r = a.access_token();
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().kind, core::ErrorKind::Auth);
    EXPECT_TRUE(http->requests().empty());
}

TEST_F(AuthFixture, FreshStoredTokenIsReturnedWithoutRefresh) {
    store_tokens(clock->now_wall() + 3000);
    auto a = auth();
    auto r = a.access_token();
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), "old-at");
    EXPECT_TRUE(http->requests().empty());
}

TEST_F(AuthFixture, ExpiredTokenIsRefreshedAndRotatedRefreshTokenStored) {
    store_tokens(clock->now_wall() + 10);  // inside the refresh skew
    http->expect("POST", "/token", 200, token_body("AT2", "RT2", 3599));
    auto a = auth();
    auto r = a.access_token();
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), "AT2");
    ASSERT_EQ(http->requests().size(), 1u);
    EXPECT_EQ(http->requests()[0].body,
              "grant_type=refresh_token&client_id=client-123&refresh_token=old-rt&scope=offline_access+User.Read+Mail.ReadWrite");
    auto ts = TokenSet::from_json(*store.get_token_blob());
    ASSERT_TRUE(ts.has_value());
    EXPECT_EQ(ts->access_token, "AT2");
    EXPECT_EQ(ts->refresh_token, "RT2");
    // cached afterwards
    ASSERT_TRUE(a.access_token().ok());
    EXPECT_EQ(http->requests().size(), 1u);
    // and refreshed again once the clock passes expiry
    http->expect("POST", "/token", 200, token_body("AT3", ""));  // no new refresh token => keep RT2
    clock->advance(3600);
    auto r3 = a.access_token();
    ASSERT_TRUE(r3.ok());
    EXPECT_EQ(r3.value(), "AT3");
    EXPECT_NE(http->requests()[1].body.find("refresh_token=RT2"), std::string::npos);
    EXPECT_EQ(TokenSet::from_json(*store.get_token_blob())->refresh_token, "RT2");
}

TEST_F(AuthFixture, InvalidateForcesRefreshEvenWhenTokenLooksFresh) {
    store_tokens(clock->now_wall() + 3000);
    http->expect("POST", "/token", 200, token_body("AT-new", "RT-new"));
    auto a = auth();
    ASSERT_EQ(a.access_token().value(), "old-at");
    a.invalidate();
    EXPECT_EQ(a.access_token().value(), "AT-new");
    EXPECT_EQ(http->requests().size(), 1u);
}

TEST_F(AuthFixture, InvalidateBeforeAnyCallStillForcesRefresh) {
    store_tokens(clock->now_wall() + 3000);
    http->expect("POST", "/token", 200, token_body("AT-new", "RT-new"));
    auto a = auth();
    a.invalidate();
    EXPECT_EQ(a.access_token().value(), "AT-new");
}

TEST_F(AuthFixture, InvalidGrantClearsBlobAndReturnsAuth) {
    store_tokens(clock->now_wall() - 5);
    http->expect("POST", "/token", 400, err_body("invalid_grant", "AADSTS70000: expired"));
    auto a = auth();
    auto r = a.access_token();
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().kind, core::ErrorKind::Auth);
    EXPECT_EQ(r.error().code, "invalid_grant");
    EXPECT_FALSE(store.get_token_blob().has_value());
    EXPECT_FALSE(a.signed_in());
    EXPECT_EQ(a.access_token().error().kind, core::ErrorKind::Auth);  // no further network calls
    EXPECT_EQ(http->requests().size(), 1u);
}

TEST_F(AuthFixture, InteractionRequiredAndFlowCheckAlsoClear) {
    store_tokens(clock->now_wall() - 5);
    http->expect("POST", "/token", 400, err_body("interaction_required", "AADSTS50076"));
    auto a = auth();
    EXPECT_EQ(a.access_token().error().kind, core::ErrorKind::Auth);
    EXPECT_FALSE(store.get_token_blob().has_value());

    store_tokens(clock->now_wall() - 5);
    http->expect("POST", "/token", 400, err_body("invalid_request", "AADSTS530036: The refresh token is invalid due to authentication flow checks"));
    auto b = auth();
    EXPECT_EQ(b.access_token().error().kind, core::ErrorKind::Auth);
    EXPECT_FALSE(store.get_token_blob().has_value());
}

TEST_F(AuthFixture, TransientRefreshFailureKeepsTokens) {
    store_tokens(clock->now_wall() - 5);
    http->expect("POST", "/token", 503, "<html/>");
    core::Error ne;
    ne.kind = core::ErrorKind::Network;
    ne.message = "down";
    http->expect_error("POST", "/token", ne);
    auto a = auth();
    EXPECT_EQ(a.access_token().error().kind, core::ErrorKind::Server);
    EXPECT_EQ(a.access_token().error().kind, core::ErrorKind::Network);
    EXPECT_TRUE(store.get_token_blob().has_value());
    EXPECT_TRUE(a.signed_in());
}

TEST_F(AuthFixture, SignOutClearsStoreAndCache) {
    store_tokens(clock->now_wall() + 3000);
    auto a = auth();
    ASSERT_TRUE(a.access_token().ok());
    EXPECT_TRUE(a.signed_in());
    ASSERT_TRUE(a.sign_out().ok());
    EXPECT_FALSE(a.signed_in());
    EXPECT_FALSE(store.get_token_blob().has_value());
    EXPECT_EQ(a.access_token().error().kind, core::ErrorKind::Auth);
}

TEST_F(AuthFixture, ErrorsNeverContainSecrets) {
    store_tokens(clock->now_wall() - 5, "SECRET-AT", "SECRET-RT");
    http->expect("POST", "/token", 400, err_body("invalid_grant", "refresh_token=SECRET-RT leaked"));
    auto a = auth();
    auto r = a.access_token();
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().message.find("SECRET"), std::string::npos);
}

TEST(StaticTokenProviderTest, ReturnsToken) {
    StaticTokenProvider p("abc");
    EXPECT_EQ(p.access_token().value(), "abc");
}
