// Foundation tests: sanitize.cpp
#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "slowmail/core/sanitize.hpp"

using namespace slowmail::core;

namespace {

std::string html(const std::string& h, std::size_t cap = 100000) {
    bool t = false;
    return html_to_text(h, cap, t);
}

bool valid_utf8(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (len == 0 || i + len > s.size()) return false;
        for (std::size_t k = 1; k < len; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        i += len;
    }
    return true;
}

bool has_control(const std::string& s) {
    for (unsigned char c : s) {
        if ((c < 0x20 && c != '\n' && c != '\t') || c == 0x7F) return true;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// html_to_text
// ---------------------------------------------------------------------------
TEST(HtmlToText, PlainParagraphsAndBreaks) {
    EXPECT_EQ(html("<p>Hello</p><p>World</p>"), "Hello\n\nWorld");
    EXPECT_EQ(html("a<br>b<br/>c"), "a\nb\nc");
    EXPECT_EQ(html("<div>one</div><div>two</div>"), "one\ntwo");
    EXPECT_EQ(html("  <b>bold</b>   and\n\t<i>italic</i>  "), "bold and italic");
    EXPECT_EQ(html(""), "");
    EXPECT_EQ(html("   \n  "), "");
}

TEST(HtmlToText, TrackingPixelRemovedNoRemoteRefs) {
    const std::string out = html(
        "<p>Hi</p><img src=\"https://tracker.example/pixel.gif?uid=42\" width=1 height=1>"
        "<link rel=stylesheet href=\"https://cdn.example/x.css\"><meta http-equiv=refresh content=\"0;url=http://e\">"
        "<base href=\"http://evil/\"><p>Bye</p>");
    EXPECT_EQ(out, "Hi\n\nBye");
    EXPECT_EQ(out.find("tracker"), std::string::npos);
    EXPECT_EQ(out.find("http"), std::string::npos);
}

TEST(HtmlToText, ScriptStyleHeadAndFriendsDropped) {
    const std::string out = html(
        "<html><head><title>SECRET TITLE</title><style>body{color:red}</style><script>alert(1)</script></head>"
        "<body>Visible"
        "<script type=text/javascript>document.write('<b>x</b>'); var a = '</div>';</script>"
        "<style>.a{}</style>"
        "<template><p>tmpl</p></template>"
        "<iframe src=http://evil><p>frame</p></iframe>"
        "<object data=x><param name=a>obj fallback</object>"
        "<embed src=evil.swf>"
        "<svg><g><text>svgtext</text></g><script>evil()</script></svg>"
        "<noscript><img src=http://t/p.gif>no js</noscript>"
        " end</body></html>");
    EXPECT_EQ(out, "Visible end");
}

TEST(HtmlToText, NestedDroppedSubtrees) {
    EXPECT_EQ(html("a<svg><svg>inner</svg>still svg</svg>b"), "ab");
    EXPECT_EQ(html("a<template><template>x</template>y</template>b"), "ab");
    EXPECT_EQ(html("a<SCRIPT>x</SCRIPT >b"), "ab");
    EXPECT_EQ(html("a<script>x</scripty>y</script>b"), "ab");  // "</scripty" does not close
}

TEST(HtmlToText, UnclosedHeadEndsAtBody) {
    EXPECT_EQ(html("<head><meta charset=utf-8><body>Hello</body>"), "Hello");
}

TEST(HtmlToText, UnterminatedConstructsAreHandled) {
    EXPECT_EQ(html("before <a href=\"http://x"), "before");               // tag never closed
    EXPECT_EQ(html("before <div class=\"abc"), "before");                  // unterminated quote
    EXPECT_EQ(html("before <!-- never closed"), "before");                 // comment
    EXPECT_EQ(html("before <![CDATA[ never closed"), "before");            // CDATA
    EXPECT_EQ(html("before <![CDATA[hidden]]> after"), "before after");
    EXPECT_EQ(html("before <script>never closed"), "before");
    EXPECT_EQ(html("before <svg><g>never closed"), "before");
    EXPECT_EQ(html("a <!DOCTYPE html> b"), "a b");
    EXPECT_EQ(html("a <?xml version=1.0?> b"), "a b");
    EXPECT_EQ(html("a <!-- c --> b <!----> c"), "a b c");
    EXPECT_EQ(html("a </"), "a");
    EXPECT_EQ(html("a <"), "a <");
    EXPECT_EQ(html("1 < 2 and 3 > 2"), "1 < 2 and 3 > 2");
    EXPECT_EQ(html("<"), "<");
    EXPECT_EQ(html("<<<<>>>>"), "<<<<>>>>");
}

TEST(HtmlToText, Entities) {
    EXPECT_EQ(html("Tom &amp; Jerry &lt;3 &gt; &quot;q&quot; &apos;a&apos;"), "Tom & Jerry <3 > \"q\" 'a'");
    EXPECT_EQ(html("a&nbsp;b"), "a b");
    EXPECT_EQ(html("&copy; &reg; &trade; &hellip; &mdash; &ndash; &euro; &pound;"),
              "\xC2\xA9 \xC2\xAE \xE2\x84\xA2 \xE2\x80\xA6 \xE2\x80\x94 \xE2\x80\x93 \xE2\x82\xAC \xC2\xA3");
    EXPECT_EQ(html("&lsquo;x&rsquo; &ldquo;y&rdquo; &bull; &middot;"),
              "\xE2\x80\x98x\xE2\x80\x99 \xE2\x80\x9Cy\xE2\x80\x9D \xE2\x80\xA2 \xC2\xB7");
    // numeric
    EXPECT_EQ(html("&#65;&#x42;&#X43;"), "ABC");
    EXPECT_EQ(html("&#233;&#xE9;"), "\xC3\xA9\xC3\xA9");
    EXPECT_EQ(html("&#x1F600;"), "\xF0\x9F\x98\x80");
    EXPECT_EQ(html("&#128512;"), "\xF0\x9F\x98\x80");
    EXPECT_EQ(html("&#150;"), "\xE2\x80\x93");  // windows-1252 mapping
    // invalid / unknown stay literal or become U+FFFD
    EXPECT_EQ(html("&bogus; &amp"), "&bogus; &amp");
    EXPECT_EQ(html("AT&T"), "AT&T");
    EXPECT_EQ(html("&#;"), "&#;");
    EXPECT_EQ(html("&#x;"), "&#x;");
    EXPECT_EQ(html("&#xD800;"), "\xEF\xBF\xBD");
    EXPECT_EQ(html("&#x110000;"), "\xEF\xBF\xBD");
    EXPECT_EQ(html("&#99999999999999999999;"), "\xEF\xBF\xBD");
    EXPECT_EQ(html("&#0;x"), "x");
    EXPECT_EQ(html("&#1;&#7;&#x1B;x"), "x");
    EXPECT_EQ(html("&#" + std::string(100000, '0') + "65;"), "A");
    EXPECT_TRUE(valid_utf8(html("&#xFFFFFFFF; &#x7FFFFFFF; &#xDFFF;")));
}

TEST(HtmlToText, EntityDecodedMarkupIsNotReparsed) {
    EXPECT_EQ(html("&lt;script&gt;alert(1)&lt;/script&gt;"), "<script>alert(1)</script>");
}

TEST(HtmlToText, NestedLists) {
    const std::string out = html("<ul><li>one</li><li>two<ul><li>two-a</li><li>two-b</li></ul></li><li>three</li></ul>");
    EXPECT_EQ(out, "- one\n- two\n  - two-a\n  - two-b\n- three");
    EXPECT_EQ(html("<ol><li>a</li><li>b</li></ol>"), "1. a\n2. b");
    EXPECT_EQ(html("intro<ul><li>x</li></ul>outro"), "intro\n\n- x\n\noutro");
}

TEST(HtmlToText, DeeplyNestedListsAreBounded) {
    std::string h;
    for (int i = 0; i < 5000; ++i) h += "<ul><li>x";
    for (int i = 0; i < 5000; ++i) h += "</li></ul>";
    const std::string out = html(h);
    EXPECT_TRUE(valid_utf8(out));
    EXPECT_FALSE(out.empty());
}

TEST(HtmlToText, TablesAndPre) {
    EXPECT_EQ(html("<table><tr><td>a</td><td>b</td></tr><tr><td>c</td><td>d</td></tr></table>"), "a b\nc d");
    EXPECT_EQ(html("<pre>line1\n  indented\nline3</pre>"), "line1\n  indented\nline3");
    EXPECT_EQ(html("a<hr>b"), "a\n----\nb");
}

TEST(HtmlToText, BlankLinesCollapsed) {
    EXPECT_EQ(html("a<br><br><br><br><br>b"), "a\n\nb");
    EXPECT_EQ(html("<p></p><p></p><p>x</p><p></p><p></p>"), "x");
    EXPECT_EQ(html("<pre>a\n\n\n\n\nb</pre>"), "a\n\nb");
}

TEST(HtmlToText, LinksHttpMailtoOnly) {
    EXPECT_EQ(html("<a href=\"https://example.com/a?b=1&amp;c=2\">click here</a>"),
              "click here <https://example.com/a?b=1&c=2>");
    EXPECT_EQ(html("<a href='http://example.com'>site</a>."), "site <http://example.com>.");
    EXPECT_EQ(html("<a href=mailto:bob@example.com>mail bob</a>"), "mail bob <mailto:bob@example.com>");
    EXPECT_EQ(html("<A HREF=\"HTTPS://EXAMPLE.COM\">up</A>"), "up <HTTPS://EXAMPLE.COM>");
    // same text as URL: not duplicated
    EXPECT_EQ(html("<a href=\"https://example.com\">https://example.com</a>"), "https://example.com");
    // image-only link keeps the URL
    EXPECT_EQ(html("<a href=\"https://example.com/x\"><img src=\"http://t/p.gif\"></a>"), "<https://example.com/x>");
}

TEST(HtmlToText, DangerousLinksDropped) {
    for (const char* href : {"javascript:alert(1)", "JaVaScRiPt:alert(1)", " javascript:alert(1)", "java\tscript:alert(1)",
                             "data:text/html,<script>x</script>", "vbscript:x", "file:///etc/passwd", "ftp://x/y",
                             "//evil.example/x", "/relative", "#frag", "http://", "https:", "tel:123", "cid:abc"}) {
        const std::string out = html(std::string("<a href=\"") + href + "\">label</a>");
        EXPECT_EQ(out, "label") << href;
    }
    EXPECT_EQ(html("<a href=javascript:alert(1)>label</a>"), "label");
    EXPECT_EQ(html("<a href=\"&#106;avascript:alert(1)\">label</a>"), "label");  // entity-obfuscated scheme
}

TEST(HtmlToText, LinkUrlsAreNeutralised) {
    const std::string out = html("<a href=\"http://example.com/a b\n<c>\">t</a>");
    EXPECT_EQ(out, "t <http://example.com/ab%3Cc%3E>");
    const std::string huge = "<a href=\"http://" + std::string(5000, 'a') + "\">t</a>";
    EXPECT_EQ(html(huge), "t");
}

TEST(HtmlToText, ControlCharsStripped) {
    std::string in = "a";
    in.push_back('\0');
    in += "b\x01\x02\x07" "c\x7F" "d\x1B[31me\xC2\x85" "f\r\ng\th";
    const std::string out = html(in);
    EXPECT_FALSE(has_control(out));
    EXPECT_EQ(out, "abcd[31mef g h");
}

TEST(HtmlToText, BidiOverridesStripped) {
    EXPECT_EQ(html("a\xE2\x80\xAE" "b\xE2\x80\x8B" "c"), "abc");
}

TEST(HtmlToText, InvalidUtf8BecomesReplacement) {
    const std::string out = html("ok \xFF\xFE bad \xC3 trunc \xE2\x82");
    EXPECT_TRUE(valid_utf8(out));
    EXPECT_NE(out.find("\xEF\xBF\xBD"), std::string::npos);
}

TEST(HtmlToText, UnicodePreserved) {
    EXPECT_EQ(html("<p>caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x98\x80</p>"), "caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x98\x80");
}

TEST(HtmlToText, TruncationOnUtf8Boundary) {
    // Each emoji is 4 bytes. Cap of 10 bytes can hold 2 emoji (8 bytes) but not a third.
    const std::string emoji = "\xF0\x9F\x98\x80";
    std::string in = "<p>";
    for (int i = 0; i < 10; ++i) in += emoji;
    in += "</p>";
    for (std::size_t cap : {0u, 1u, 3u, 4u, 5u, 7u, 8u, 9u, 10u, 11u, 39u, 40u, 41u}) {
        bool t = false;
        const std::string out = html_to_text(in, cap, t);
        EXPECT_LE(out.size(), cap);
        EXPECT_TRUE(valid_utf8(out)) << cap;
        EXPECT_EQ(out.size() % 4, 0u) << cap;
        EXPECT_EQ(t, cap < 40u) << "cap " << cap;
    }
    bool t = false;
    EXPECT_EQ(html_to_text(in, 10, t), emoji + emoji);
    EXPECT_TRUE(t);
}

TEST(HtmlToText, TruncationFlagNotSetForTrailingWhitespace) {
    bool t = true;
    EXPECT_EQ(html_to_text("abc<br><br>   ", 3, t), "abc");
    EXPECT_FALSE(t);
    EXPECT_EQ(html_to_text("abcd", 3, t), "abc");
    EXPECT_TRUE(t);
    EXPECT_EQ(html_to_text("abc", 3, t), "abc");
    EXPECT_FALSE(t);
    EXPECT_EQ(html_to_text("abc", 0, t), "");
    EXPECT_TRUE(t);
}

TEST(HtmlToText, OneMegabyteInputIsFast) {
    std::string big;
    big.reserve(1 << 20);
    while (big.size() < (1u << 20)) {
        big += "<div class=\"x\"><p>Paragraph &amp; text with <b>bold</b> and <a href=\"https://e.com/?a=1&amp;b=2\">link</a>.</p>"
               "<ul><li>item</li></ul><script>var x = 1 < 2;</script><img src=x></div>\n";
    }
    const auto t0 = std::chrono::steady_clock::now();
    bool truncated = false;
    const std::string out = html_to_text(big, 200000, truncated);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    EXPECT_LT(ms, 1000);
    EXPECT_TRUE(truncated);
    EXPECT_LE(out.size(), 200000u);
    EXPECT_TRUE(valid_utf8(out));
    EXPECT_EQ(out.find("var x"), std::string::npos);
}

TEST(HtmlToText, PathologicalInputsAreLinearAndSafe) {
    const auto t0 = std::chrono::steady_clock::now();
    bool t = false;
    // many unterminated openers
    std::string a;
    for (int i = 0; i < 100000; ++i) a += "<a href=\"x\"";
    (void)html_to_text(a, 1000, t);
    // many nested elements that never close
    std::string b;
    for (int i = 0; i < 100000; ++i) b += "<svg>";
    (void)html_to_text(b, 1000, t);
    // many script starts
    std::string c;
    for (int i = 0; i < 100000; ++i) c += "<script>";
    (void)html_to_text(c, 1000, t);
    // many "<!--" without terminator and "<" alone
    std::string d;
    for (int i = 0; i < 100000; ++i) d += "<!-- ";
    (void)html_to_text(d, 1000, t);
    std::string e(500000, '<');
    (void)html_to_text(e, 1000, t);
    // many entities
    std::string f;
    for (int i = 0; i < 200000; ++i) f += "&amp;&#x41;&bogus;";
    (void)html_to_text(f, 1 << 20, t);
    // many links
    std::string g;
    for (int i = 0; i < 50000; ++i) g += "<a href=http://e.com/>x";
    (void)html_to_text(g, 1 << 20, t);
    // random-ish binary
    std::string r;
    unsigned x = 12345;
    for (int i = 0; i < 300000; ++i) {
        x = x * 1664525u + 1013904223u;
        r.push_back(static_cast<char>(x >> 24));
    }
    const std::string out = html_to_text(r, 1 << 20, t);
    EXPECT_TRUE(valid_utf8(out));
    EXPECT_FALSE(has_control(out));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    EXPECT_LT(ms, 3000);
}

TEST(HtmlToText, EveryPrefixOfADocumentIsSafe) {
    const std::string doc =
        "<html><head><style>a{}</style></head><body><!-- c --><p class=\"a\">Hi &amp; <a href=\"https://x.y/?q=1&amp;z\">l</a>"
        "</p><![CDATA[x]]><ul><li>a<li>b</ul><script>if(a<b){}</script><svg><g/></svg>\xF0\x9F\x98\x80</body></html>";
    for (std::size_t n = 0; n <= doc.size(); ++n) {
        bool t = false;
        const std::string out = html_to_text(doc.substr(0, n), 100, t);
        ASSERT_TRUE(valid_utf8(out)) << n;
        ASSERT_LE(out.size(), 100u);
    }
}

// ---------------------------------------------------------------------------
// sanitize_text / sanitize_line / utf8_truncate
// ---------------------------------------------------------------------------
TEST(SanitizeText, StripsControlsKeepsNewlineAndTab) {
    bool t = false;
    std::string in = "a\x01" "b\r\nc\rd\te\x7F\n\x1B" "f";
    EXPECT_EQ(sanitize_text(in, 100, t), "ab\nc\nd\te\nf");
    EXPECT_FALSE(t);
}

TEST(SanitizeText, CapsOnUtf8Boundary) {
    bool t = false;
    const std::string s = "ab\xE2\x82\xAC" "cd";  // a b EURO c d
    EXPECT_EQ(sanitize_text(s, 3, t), "ab");
    EXPECT_TRUE(t);
    EXPECT_EQ(sanitize_text(s, 5, t), "ab\xE2\x82\xAC");
    EXPECT_TRUE(t);
    EXPECT_EQ(sanitize_text(s, 7, t), s);
    EXPECT_FALSE(t);
}

TEST(SanitizeText, InvalidUtf8Replaced) {
    bool t = false;
    const std::string out = sanitize_text("x\xFF\xC0\x80y", 100, t);
    EXPECT_TRUE(valid_utf8(out));
    EXPECT_EQ(out.front(), 'x');
    EXPECT_EQ(out.back(), 'y');
}

TEST(SanitizeLine, BasicsAndInjection) {
    EXPECT_EQ(sanitize_line("  Hello \r\n World\t!  ", 100), "Hello World !");
    EXPECT_EQ(sanitize_line("he\x01llo\x07", 100), "hello");
    EXPECT_EQ(sanitize_line("Subject\r\nBcc: evil@example.com", 100), "Subject Bcc: evil@example.com");
    EXPECT_EQ(sanitize_line("", 10), "");
    EXPECT_EQ(sanitize_line("abc", 0), "");
}

TEST(SanitizeLine, CapIsUtf8SafeAndTrimmed) {
    EXPECT_EQ(sanitize_line("abcdef", 3), "abc");
    EXPECT_EQ(sanitize_line("ab\xF0\x9F\x98\x80", 5), "ab");
    EXPECT_EQ(sanitize_line("ab\xF0\x9F\x98\x80", 6), "ab\xF0\x9F\x98\x80");
    EXPECT_EQ(sanitize_line("ab cd", 3), "ab");  // never ends with a dangling space
    std::string longs(100000, 'x');
    EXPECT_EQ(sanitize_line(longs, Limits::kMaxSubject).size(), Limits::kMaxSubject);
}

TEST(Utf8Truncate, Boundaries) {
    const std::string s = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";  // 1+2+3+4 bytes
    EXPECT_EQ(utf8_truncate(s, 100), s);
    EXPECT_EQ(utf8_truncate(s, 10), s);
    EXPECT_EQ(utf8_truncate(s, 9), "a\xC3\xA9\xE2\x82\xAC");
    EXPECT_EQ(utf8_truncate(s, 7), "a\xC3\xA9\xE2\x82\xAC");
    EXPECT_EQ(utf8_truncate(s, 6), "a\xC3\xA9\xE2\x82\xAC");
    EXPECT_EQ(utf8_truncate(s, 5), "a\xC3\xA9");
    EXPECT_EQ(utf8_truncate(s, 4), "a\xC3\xA9");
    EXPECT_EQ(utf8_truncate(s, 3), "a\xC3\xA9");
    EXPECT_EQ(utf8_truncate(s, 2), "a");
    EXPECT_EQ(utf8_truncate(s, 1), "a");
    EXPECT_EQ(utf8_truncate(s, 0), "");
    EXPECT_EQ(utf8_truncate("", 0), "");
    for (std::size_t n = 0; n <= s.size() + 1; ++n) EXPECT_TRUE(valid_utf8(utf8_truncate(s, n))) << n;
}

// ---------------------------------------------------------------------------
// attachments
// ---------------------------------------------------------------------------
TEST(SanitizeAttachment, HugeNameIsBoundedAndSafe) {
    AttachmentMeta a;
    a.id = "AAMk";
    a.name = std::string(10000, 'n') + "\xF0\x9F\x98\x80";
    a.content_type = "application/pdf";
    a.size = 2048;
    a.kind = AttachmentKind::File;
    const AttachmentMeta s = sanitize_attachment(a);
    EXPECT_EQ(s.name.size(), Limits::kMaxAttachmentName);
    EXPECT_TRUE(valid_utf8(s.name));
    EXPECT_EQ(s.content_type, "application/pdf");
    EXPECT_EQ(s.size, 2048);
    EXPECT_EQ(s.kind, AttachmentKind::File);
}

TEST(SanitizeAttachment, HugeNameCutInsideMultibyteChar) {
    AttachmentMeta a;
    a.name = std::string(Limits::kMaxAttachmentName - 1, 'n') + "\xE2\x82\xAC";  // euro straddles the cap
    const AttachmentMeta s = sanitize_attachment(a);
    EXPECT_EQ(s.name.size(), Limits::kMaxAttachmentName - 1);
    EXPECT_TRUE(valid_utf8(s.name));
}

TEST(SanitizeAttachment, NegativeAndUnknownSizes) {
    AttachmentMeta a;
    a.name = "x";
    a.size = -12345;
    EXPECT_EQ(sanitize_attachment(a).size, -1);
    a.size = -1;
    EXPECT_EQ(sanitize_attachment(a).size, -1);
    a.size = 0;
    EXPECT_EQ(sanitize_attachment(a).size, 0);
    a.size = INT64_MIN;
    EXPECT_EQ(sanitize_attachment(a).size, -1);
    a.size = INT64_MAX;
    EXPECT_EQ(sanitize_attachment(a).size, INT64_MAX);
}

TEST(SanitizeAttachment, ControlCharsInContentTypeAndName) {
    AttachmentMeta a;
    a.name = "inv\x01oice\r\n.pdf\x1B";
    a.content_type = std::string("application/pdf\r\nX-Evil: 1\x00", 28);
    const AttachmentMeta s = sanitize_attachment(a);
    EXPECT_EQ(s.name, "invoice .pdf");
    EXPECT_EQ(s.content_type, "application/pdf");
    EXPECT_FALSE(has_control(s.content_type));

    a.content_type = "TEXT/Plain; charset=utf-8";
    EXPECT_EQ(sanitize_attachment(a).content_type, "text/plain");
    for (const char* ct : {"", "pdf", "/", "a/", "/b", "a/b/c", "a b/c", "<script>/x", "application/\x01\x02", "\r\n"}) {
        a.content_type = ct;
        EXPECT_EQ(sanitize_attachment(a).content_type, "application/octet-stream") << ct;
    }
}

TEST(SanitizeAttachment, EmptyNameGetsPlaceholder) {
    AttachmentMeta a;
    EXPECT_EQ(sanitize_attachment(a).name, "(unnamed)");
    a.name = "\x01\x02";
    EXPECT_EQ(sanitize_attachment(a).name, "(unnamed)");
}

TEST(SanitizeAttachment, ListIsBounded) {
    std::vector<AttachmentMeta> raw(Limits::kMaxAttachmentsListed + 50);
    for (std::size_t i = 0; i < raw.size(); ++i) {
        raw[i].id = "id" + std::to_string(i);
        raw[i].name = std::string(100000, 'x');
        raw[i].size = -5;
    }
    const auto out = sanitize_attachments(raw);
    ASSERT_EQ(out.size(), Limits::kMaxAttachmentsListed);
    EXPECT_EQ(out[0].id, "id0");
    EXPECT_EQ(out.back().id, "id" + std::to_string(Limits::kMaxAttachmentsListed - 1));
    EXPECT_EQ(out[5].name.size(), Limits::kMaxAttachmentName);
    EXPECT_EQ(out[5].size, -1);
    EXPECT_TRUE(sanitize_attachments({}).empty());
}

TEST(HumanSize, Formatting) {
    EXPECT_EQ(human_size(-1), "unknown size");
    EXPECT_EQ(human_size(-999), "unknown size");
    EXPECT_EQ(human_size(0), "0 B");
    EXPECT_EQ(human_size(1), "1 B");
    EXPECT_EQ(human_size(1023), "1023 B");
    EXPECT_EQ(human_size(1024), "1 KB");
    EXPECT_EQ(human_size(1536), "1.5 KB");
    EXPECT_EQ(human_size(12 * 1024), "12 KB");
    EXPECT_EQ(human_size(1024 * 1024 * 14 / 10), "1.4 MB");
    EXPECT_EQ(human_size(5LL * 1024 * 1024), "5 MB");
    EXPECT_EQ(human_size(3LL * 1024 * 1024 * 1024), "3 GB");
    EXPECT_EQ(human_size(1023 * 1024 + 900), "1 MB");  // rounds up into the next unit
    EXPECT_NE(human_size(INT64_MAX).find("TB"), std::string::npos);
}
