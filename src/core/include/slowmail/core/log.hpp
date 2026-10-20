#pragma once
#include <functional>
#include <string>

namespace slowmail::core {

// Redacting logger. Rule (handover invariant 9): no credentials, tokens, subjects, bodies
// or addresses in logs. Callers log message IDs and counts, never content.
// redact() scrubs anything that looks like a bearer token, an email address or a JWT as a safety net.
enum class LogLevel { Debug, Info, Warn, Error };

using LogSink = std::function<void(LogLevel, const std::string&)>;

void set_log_sink(LogSink sink);       // default: stderr
void log(LogLevel level, const std::string& message);
std::string redact(const std::string& s);

#define SLOWMAIL_LOG_DEBUG(msg) ::slowmail::core::log(::slowmail::core::LogLevel::Debug, (msg))
#define SLOWMAIL_LOG_INFO(msg)  ::slowmail::core::log(::slowmail::core::LogLevel::Info, (msg))
#define SLOWMAIL_LOG_WARN(msg)  ::slowmail::core::log(::slowmail::core::LogLevel::Warn, (msg))
#define SLOWMAIL_LOG_ERROR(msg) ::slowmail::core::log(::slowmail::core::LogLevel::Error, (msg))

} // namespace slowmail::core
