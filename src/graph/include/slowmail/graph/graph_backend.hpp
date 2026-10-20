#pragma once
#include <memory>
#include <string>

#include "slowmail/core/clock.hpp"
#include "slowmail/core/mail_backend.hpp"
#include "slowmail/graph/auth.hpp"
#include "slowmail/graph/http_transport.hpp"

namespace slowmail::graph {

struct GraphConfig {
    std::string base_url = "https://graph.microsoft.com/v1.0";  // overridable for the fake server
    bool prefer_immutable_ids = true;        // Prefer: IdType="ImmutableId"
    int page_size = 50;                      // Prefer: odata.maxpagesize
    int max_retries_on_throttle = 3;         // honour Retry-After, cap waits
    int max_retry_after_wait_seconds = 60;   // longer waits are returned to the caller as Throttled
    bool sleep_on_retry = true;              // tests set false
};

// Microsoft Graph implementation of core::MailBackend.
// Delta:   GET {base}/me/mailFolders/inbox/messages/delta?$select=id,conversationId,subject,from,receivedDateTime,isRead,flag,bodyPreview,hasAttachments,changeKey,parentFolderId
//          then follow @odata.nextLink / @odata.deltaLink verbatim. @removed entries -> removed_ids.
// Body:    GET {base}/me/messages/{id}?$select=id,body,bodyPreview,hasAttachments   with Prefer: outlook.body-content-type="text"
//          then GET {base}/me/messages/{id}/attachments?$select=id,name,contentType,size,isInline  (metadata only, NEVER contentBytes)
// Move:    POST {base}/me/messages/{id}/move  {"destinationId":"archive"|"deleteditems"|"inbox"}
// Flag:    PATCH {base}/me/messages/{id}      {"flag":{"flagStatus":"flagged"|"notFlagged"}}
// Account: GET {base}/me?$select=id,userPrincipalName,mail
// Error mapping: 401->Auth (after one refresh retry), 403->Forbidden, 404->NotFound, 409/412->Conflict,
//                410->Gone, 429/503(+Retry-After)->Throttled, other 5xx->Server, parse failure->Malformed.
// All responses are sanitised through core/sanitize before leaving this class.
class GraphBackend final : public core::MailBackend {
public:
    GraphBackend(GraphConfig config, std::shared_ptr<HttpTransport> transport,
                 std::shared_ptr<TokenProvider> tokens, std::shared_ptr<core::Clock> clock);

    core::Outcome<core::DeltaPage> fetch_inbox_delta(const std::string& link) override;
    core::Outcome<core::MessageBody> fetch_body(const std::string& message_id) override;
    core::Outcome<core::MoveResult> move(const std::string& message_id, core::WellKnownFolder destination) override;
    core::Status set_flag(const std::string& message_id, core::FlagStatus status) override;
    core::Outcome<core::AccountInfo> account() override;

    const GraphConfig& config() const { return config_; }

    // Exposed for unit tests: pure parsers.
    static core::Outcome<core::DeltaPage> parse_delta_page(const std::string& json);
    static core::Outcome<core::MessageSummary> parse_message_summary(const nlohmann::json& j);
    static core::Error map_http_error(const HttpResponse& r);

private:
    GraphConfig config_;
    std::shared_ptr<HttpTransport> transport_;
    std::shared_ptr<TokenProvider> tokens_;
    std::shared_ptr<core::Clock> clock_;
    core::Outcome<HttpResponse> call(const std::string& method, const std::string& url, const std::string& body,
                                     const std::map<std::string, std::string>& extra_headers);
};

} // namespace slowmail::graph
