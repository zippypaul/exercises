#include <gtest/gtest.h>

#include "../../../src/graph/src/url.hpp"

using namespace slowmail::graph::url;

TEST(UrlTest, PercentEncodeKeepsUnreservedOnly) {
    EXPECT_EQ(percent_encode("AZaz09-_.~"), "AZaz09-_.~");
    EXPECT_EQ(percent_encode("a b/c+d=e"), "a%20b%2Fc%2Bd%3De");
    EXPECT_EQ(percent_encode("\xC3\xA9"), "%C3%A9");
    EXPECT_EQ(percent_encode("a,b$c", ",$"), "a,b$c");
}

TEST(UrlTest, PathSegmentEncodesSlashAndPadding) {
    EXPECT_EQ(encode_path_segment("AAMk/AD+x=="), "AAMk%2FAD%2Bx%3D%3D");
}

TEST(UrlTest, FormEncodeUsesPlusForSpace) {
    EXPECT_EQ(form_encode({{"client_id", "abc"}, {"scope", "offline_access User.Read"}}),
              "client_id=abc&scope=offline_access+User.Read");
    EXPECT_EQ(form_encode({{"grant_type", "urn:ietf:params:oauth:grant-type:device_code"}}),
              "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code");
    EXPECT_EQ(form_encode({{"k", "a&b=c%d"}}), "k=a%26b%3Dc%25d");
    EXPECT_EQ(form_encode({}), "");
}

TEST(UrlTest, AppendQueryChoosesSeparatorAndKeepsODataSyntax) {
    EXPECT_EQ(append_query("https://x/y", "$select", "id,subject"), "https://x/y?$select=id,subject");
    EXPECT_EQ(append_query("https://x/y?a=1", "$filter", "a ge 1"), "https://x/y?a=1&$filter=a%20ge%201");
}

TEST(UrlTest, OriginOf) {
    EXPECT_EQ(origin_of("https://Graph.Microsoft.com/v1.0/me?x=1"), "https://graph.microsoft.com");
    EXPECT_EQ(origin_of("https://graph.microsoft.com@evil.example/x"), "https://graph.microsoft.com@evil.example");
    EXPECT_EQ(origin_of("http://127.0.0.1:8080"), "http://127.0.0.1:8080");
    EXPECT_EQ(origin_of("not a url"), "");
}
