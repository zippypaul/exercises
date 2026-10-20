#include "triage_controller.hpp"

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMetaObject>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

#include "slowmail/core/triage.hpp"

namespace slowmail::ui {

namespace {

QString q(const std::string& s) { return QString::fromUtf8(s.data(), static_cast<int>(s.size())); }

QString describeError(const core::Error& e) {
    QString head;
    switch (e.kind) {
        case core::ErrorKind::Network:
            head = QStringLiteral("No connection. Changes are kept and sent when you are back online.");
            break;
        case core::ErrorKind::Auth:
            head = QStringLiteral("Sign-in required.");
            break;
        case core::ErrorKind::Forbidden:
            head = QStringLiteral("Access denied by Microsoft 365 (consent or conditional access).");
            break;
        case core::ErrorKind::Throttled:
            head = QStringLiteral("Microsoft 365 asked us to slow down. Try again later.");
            if (e.retry_after_seconds) head += QStringLiteral(" (retry in %1 s)").arg(*e.retry_after_seconds);
            break;
        case core::ErrorKind::NotFound:
            head = QStringLiteral("That message no longer exists on the server.");
            break;
        case core::ErrorKind::Storage:
            head = QStringLiteral("Local storage error.");
            break;
        default:
            break;
    }
    const QString detail = q(e.message);
    if (head.isEmpty()) return detail.isEmpty() ? QStringLiteral("Something went wrong.") : detail;
    return detail.isEmpty() ? head : head + QLatin1Char(' ') + detail;
}

// Poll/begin refused for policy reasons (Conditional Access, app registration, consent): show a distinct message
// with the AADSTS code when the adapter supplied one. The code is only shown if it has the expected shape.
QString signInBlockedMessage(const core::Error& e) {
    QString msg = QStringLiteral("Your organisation blocks this sign-in method");
    const QString code = q(e.code);
    static const QRegularExpression re(QStringLiteral("^AADSTS[0-9]{1,8}$"));
    if (re.match(code).hasMatch()) msg += QStringLiteral(" (%1)").arg(code);
    return msg + QStringLiteral(". Ask your admin; there is no workaround in Slow Mail.");
}

QString formatNextEligible(std::int64_t epoch) {
    const QDateTime dt = QDateTime::fromSecsSinceEpoch(epoch);
    if (dt.date() == QDate::currentDate()) return dt.toString(QStringLiteral("HH:mm"));
    return QLocale::c().toString(dt, QStringLiteral("ddd HH:mm"));
}

QString formatReceived(const core::MessageSummary& m) {
    if (m.received_at_epoch <= 0) return q(m.received_at_iso);
    const QDateTime dt = QDateTime::fromSecsSinceEpoch(m.received_at_epoch);
    if (dt.date() == QDate::currentDate()) return dt.toString(QStringLiteral("HH:mm"));
    if (dt.date().year() == QDate::currentDate().year()) return QLocale::c().toString(dt, QStringLiteral("d MMM HH:mm"));
    return QLocale::c().toString(dt, QStringLiteral("d MMM yyyy"));
}

struct EventLabel {
    const char* key;
    const char* label;
    int rank;
};

bool eventLabel(core::EventType t, EventLabel& out) {
    switch (t) {
        case core::EventType::Archived: out = {"archived", "Archived", 0}; return true;
        case core::EventType::Deleted: out = {"deleted", "Deleted", 1}; return true;
        case core::EventType::Deferred: out = {"deferred", "Deferred", 2}; return true;
        case core::EventType::Undeferred: out = {"undeferred", "Restored to queue", 3}; return true;
        case core::EventType::UndoArchive: out = {"undo_archive", "Archive undone", 4}; return true;
        case core::EventType::UndoDelete: out = {"undo_delete", "Delete undone", 5}; return true;
        case core::EventType::Sent: out = {"sent", "Sent", 6}; return true;
        case core::EventType::Delegated: out = {"delegated", "Delegated", 7}; return true;
        default: return false;   // sync events are not shown in the activity view
    }
}

QString confirmationLabel(core::ConfirmationState c) {
    switch (c) {
        case core::ConfirmationState::Pending: return QStringLiteral("Pending");
        case core::ConfirmationState::Confirmed: return QStringLiteral("Confirmed");
        case core::ConfirmationState::Failed: return QStringLiteral("Failed");
    }
    return QStringLiteral("Pending");
}

struct JobGuard {
    explicit JobGuard(TriageWorker* w) : w_(w) {}
    ~JobGuard() { emit w_->jobFinished(); }
    TriageWorker* w_;
};

} // namespace

// ---------------------------------------------------------------------------------------------------------------
// RowModel
// ---------------------------------------------------------------------------------------------------------------
RowModel::RowModel(const QList<QByteArray>& roles, QObject* parent) : QAbstractListModel(parent), roles_(roles) {}

int RowModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : static_cast<int>(rows_.size()); }

QVariant RowModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size()) return {};
    const int r = role - (Qt::UserRole + 1);
    if (r < 0 || r >= roles_.size()) return {};
    return rows_.at(index.row()).value(QString::fromLatin1(roles_.at(r)));
}

QHash<int, QByteArray> RowModel::roleNames() const {
    QHash<int, QByteArray> h;
    for (int i = 0; i < roles_.size(); ++i) h.insert(Qt::UserRole + 1 + i, roles_.at(i));
    return h;
}

QVariantMap RowModel::get(int row) const {
    if (row < 0 || row >= rows_.size()) return {};
    return rows_.at(row);
}

void RowModel::setRows(const QVector<QVariantMap>& rows) {
    const int before = static_cast<int>(rows_.size());
    const QString key = QStringLiteral("id");

    QSet<QString> newIds;
    for (const auto& r : rows) newIds.insert(r.value(key).toString());

    // 1. rows that vanished
    for (int i = static_cast<int>(rows_.size()) - 1; i >= 0; --i) {
        if (!newIds.contains(rows_.at(i).value(key).toString())) {
            beginRemoveRows(QModelIndex(), i, i);
            rows_.remove(i);
            endRemoveRows();
        }
    }
    // 2. walk the new order, updating in place and inserting what is new
    QSet<QString> have;
    for (const auto& r : rows_) have.insert(r.value(key).toString());
    for (int i = 0; i < rows.size(); ++i) {
        const QString id = rows.at(i).value(key).toString();
        if (i < rows_.size() && rows_.at(i).value(key).toString() == id) {
            if (rows_.at(i) != rows.at(i)) {
                rows_[i] = rows.at(i);
                emit dataChanged(index(i), index(i));
            }
        } else if (have.contains(id)) {
            // Order changed in a way we do not diff: fall back to a reset.
            beginResetModel();
            rows_ = rows;
            endResetModel();
            if (before != rows_.size()) emit countChanged();
            return;
        } else {
            beginInsertRows(QModelIndex(), i, i);
            rows_.insert(i, rows.at(i));
            endInsertRows();
            have.insert(id);
        }
    }
    if (before != rows_.size()) emit countChanged();
}

void RowModel::removeById(const QString& id) {
    for (int i = 0; i < rows_.size(); ++i) {
        if (rows_.at(i).value(QStringLiteral("id")).toString() == id) {
            beginRemoveRows(QModelIndex(), i, i);
            rows_.remove(i);
            endRemoveRows();
            emit countChanged();
            return;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// TriageWorker (worker thread)
// ---------------------------------------------------------------------------------------------------------------
TriageWorker::TriageWorker(app::AppContext* ctx, QObject* parent) : QObject(parent), ctx_(ctx) {}

Snapshot TriageWorker::makeSnapshot() {
    Snapshot s;
    core::TriageSession& session = ctx_->session();
    const core::StatusSnapshot st = session.status();
    s.sessionState = q(core::to_string(st.session_state));
    s.signedIn = st.signed_in;
    s.offline = st.offline;
    s.pendingCount = static_cast<int>(st.pending_commands);
    s.deferredCount = static_cast<int>(st.deferred_count);
    s.failedCount = static_cast<int>(st.failed_count);
    s.lastError = q(st.last_error);
    s.refreshEligible = st.cooldown.eligible;
    if (st.cooldown.last_success_wall) {
        s.downloadedAt = q(app::format_local_hhmm(*st.cooldown.last_success_wall));
    }
    if (!st.cooldown.eligible && st.cooldown.next_eligible_wall) {
        s.nextEligibleAt = formatNextEligible(*st.cooldown.next_eligible_wall);
    }

    for (const core::MessageSummary& m : session.queue()) {
        QVariantMap row;
        row.insert(QStringLiteral("id"), q(m.id));
        row.insert(QStringLiteral("subject"), q(m.subject));
        row.insert(QStringLiteral("fromName"), (m.from.name.empty() && m.from.address.empty()) ? QStringLiteral("(unknown sender)") : q(m.from.name.empty() ? m.from.address : m.from.name));
        row.insert(QStringLiteral("fromAddress"), q(m.from.address));
        row.insert(QStringLiteral("receivedAt"), formatReceived(m));
        row.insert(QStringLiteral("preview"), q(m.body_preview));
        row.insert(QStringLiteral("hasAttachments"), m.has_attachments);
        row.insert(QStringLiteral("isRead"), m.is_read);
        row.insert(QStringLiteral("flagged"), m.flag_status == core::FlagStatus::Flagged);
        s.queue.push_back(row);
    }

    for (const core::MessageSummary& m : session.failed()) {
        QString action;
        if (const auto c = session.failed_command(m.id)) {
            switch (c->type) {
                case core::CommandType::Archive: action = QStringLiteral("Archive"); break;
                case core::CommandType::Delete: action = QStringLiteral("Delete"); break;
                case core::CommandType::Defer: action = QStringLiteral("Defer"); break;
                case core::CommandType::Undefer: action = QStringLiteral("Restore"); break;
                case core::CommandType::UndoArchive: action = QStringLiteral("Undo archive"); break;
                case core::CommandType::UndoDelete: action = QStringLiteral("Undo delete"); break;
            }
        }
        QVariantMap row;
        row.insert(QStringLiteral("id"), q(m.id));
        row.insert(QStringLiteral("subject"), q(m.subject));
        row.insert(QStringLiteral("from"), (m.from.name.empty() && m.from.address.empty()) ? QStringLiteral("(unknown sender)") : q(m.from.name.empty() ? m.from.address : m.from.name));
        row.insert(QStringLiteral("action"), action);
        row.insert(QStringLiteral("reason"), q(session.failure_reason(m.id)));
        s.failed.push_back(row);
    }

    struct Tmp {
        int rank;
        std::int64_t ts;
        QVariantMap row;
    };
    std::vector<Tmp> tmp;
    const std::vector<core::ActivityRow> rows = session.activity();
    for (const core::ActivityRow& a : rows) {
        EventLabel lab{};
        if (!eventLabel(a.event.type, lab)) continue;
        bool canUndo = (a.event.type == core::EventType::Archived || a.event.type == core::EventType::Deleted) &&
                       a.event.confirmation == core::ConfirmationState::Confirmed && !a.event.command_id.empty();
        if (canUndo) {
            for (const core::ActivityRow& b : rows) {
                if (b.event.message_id == a.event.message_id && b.event.timestamp >= a.event.timestamp &&
                    (b.event.type == core::EventType::UndoArchive || b.event.type == core::EventType::UndoDelete)) {
                    canUndo = false;
                    break;
                }
            }
        }
        QVariantMap row;
        row.insert(QStringLiteral("id"), q(a.event.id));
        row.insert(QStringLiteral("type"), QString::fromLatin1(lab.label));
        row.insert(QStringLiteral("typeKey"), QString::fromLatin1(lab.key));
        row.insert(QStringLiteral("subject"), q(a.subject));
        row.insert(QStringLiteral("from"), (a.from_name.empty() && a.from_address.empty()) ? QStringLiteral("(unknown sender)") : q(a.from_name.empty() ? a.from_address : a.from_name));
        row.insert(QStringLiteral("time"),
                   QDateTime::fromSecsSinceEpoch(a.event.timestamp).toString(QStringLiteral("HH:mm")));
        row.insert(QStringLiteral("confirmation"), confirmationLabel(a.event.confirmation));
        row.insert(QStringLiteral("failureReason"), q(a.event.failure_reason));
        row.insert(QStringLiteral("commandId"), q(a.event.command_id));
        row.insert(QStringLiteral("messageId"), q(a.event.message_id));
        row.insert(QStringLiteral("canUndo"), canUndo);
        tmp.push_back({lab.rank, a.event.timestamp, row});
    }
    std::stable_sort(tmp.begin(), tmp.end(), [](const Tmp& x, const Tmp& y) {
        if (x.rank != y.rank) return x.rank < y.rank;
        return x.ts > y.ts;
    });
    for (const Tmp& t : tmp) s.activity.push_back(t.row);
    return s;
}

void TriageWorker::emitSnapshot() { emit snapshotReady(makeSnapshot()); }

void TriageWorker::start() {
    JobGuard g(this);
    const core::Status st = ctx_->session().start();
    if (!st.ok()) emit errorOccurred(describeError(st.error()));
    emitSnapshot();
}

void TriageWorker::resnapshot() {
    JobGuard g(this);
    emitSnapshot();
}

void TriageWorker::refresh(bool force) {
    JobGuard g(this);
    auto r = ctx_->session().refresh(force);
    if (!r.ok()) {
        if (r.error().kind == core::ErrorKind::Policy) {
            emit notice(QStringLiteral("Refresh is not available yet."));
        } else {
            emit errorOccurred(describeError(r.error()));
        }
    } else if (!r.value().complete) {
        emit errorOccurred(describeError(r.value().error));
    } else {
        emit errorOccurred(QString());
    }
    emitSnapshot();
}

void TriageWorker::flush() {
    JobGuard g(this);
    const core::ProcessReport r = ctx_->session().flush();
    if (r.auth_required) emit errorOccurred(QStringLiteral("Sign-in required."));
    else if (r.went_offline) {
        core::Error e;
        e.kind = core::ErrorKind::Network;
        emit errorOccurred(describeError(e));
    }
    else if (r.failed > 0) emit errorOccurred(QStringLiteral("%1 action(s) failed. See Activity.").arg(r.failed));
    else emit errorOccurred(QString());
    emitSnapshot();
}

void TriageWorker::retry(const QString& messageId) {
    JobGuard g(this);
    actionResult(ctx_->session().retry(messageId.toStdString()));
    emitSnapshot();
}

void TriageWorker::open(const QString& id) {
    JobGuard g(this);
    auto r = ctx_->session().open(id.toStdString());
    if (!r.ok()) {
        const QString reason = describeError(r.error());
        emit messageOpenFailed(id, reason);
        emit errorOccurred(reason);
    } else {
        const core::MessageBody& b = r.value();
        QJsonArray arr;
        for (const core::AttachmentMeta& a : b.attachments) {
            QJsonObject o;
            o.insert(QStringLiteral("name"), q(a.name));
            o.insert(QStringLiteral("type"), q(a.content_type.empty() ? std::string(core::to_string(a.kind)) : a.content_type));
            o.insert(QStringLiteral("size"), q(app::format_size(a.size)));
            o.insert(QStringLiteral("inline"), a.is_inline);
            arr.append(o);
        }
        emit messageOpened(id, q(b.text), QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)), b.truncated);
    }
    emitSnapshot();
}

void TriageWorker::actionResult(const core::Outcome<core::Command>& r) {
    if (!r.ok()) {
        emit errorOccurred(describeError(r.error()));
        return;
    }
    const core::Command& c = r.value();
    if (c.state == core::CommandState::Failed) {
        emit errorOccurred(QStringLiteral("Action failed: ") + q(c.last_error));
    } else if (c.state == core::CommandState::Pending || c.state == core::CommandState::InFlight) {
        emit errorOccurred(QString());
        emit notice(QStringLiteral("Saved. It will be sent when you are online."));
    } else {
        emit errorOccurred(QString());
    }
}

void TriageWorker::archive(const QString& id) {
    JobGuard g(this);
    actionResult(ctx_->session().archive(id.toStdString()));
    emitSnapshot();
}
void TriageWorker::remove(const QString& id) {
    JobGuard g(this);
    actionResult(ctx_->session().remove(id.toStdString()));
    emitSnapshot();
}
void TriageWorker::defer(const QString& id) {
    JobGuard g(this);
    actionResult(ctx_->session().defer(id.toStdString()));
    emitSnapshot();
}
void TriageWorker::undefer(const QString& id) {
    JobGuard g(this);
    actionResult(ctx_->session().undefer(id.toStdString()));
    emitSnapshot();
}
void TriageWorker::undo(const QString& commandId) {
    JobGuard g(this);
    actionResult(ctx_->session().undo(commandId.toStdString()));
    emitSnapshot();
}

void TriageWorker::beginSignIn() {
    JobGuard g(this);
    stopPolling();
    auto r = ctx_->auth()->begin();
    if (!r.ok()) {
        const QString msg = r.error().kind == core::ErrorKind::Forbidden ? signInBlockedMessage(r.error()) : describeError(r.error());
        emit signInFinished(false, msg);
        emit errorOccurred(msg);
        return;
    }
    challenge_ = r.value();
    pollIntervalSec_ = qBound<int>(1, challenge_.interval > 0 ? challenge_.interval : 5, 300);
    deadlineMs_ = QDateTime::currentMSecsSinceEpoch() + static_cast<qint64>(challenge_.expires_in > 0 ? challenge_.expires_in : 900) * 1000;
    signingIn_ = true;
    emit errorOccurred(QString());
    emit deviceCodeReady(q(challenge_.user_code), q(challenge_.verification_uri));
    if (!pollTimer_) {
        pollTimer_ = new QTimer(this);   // created on the worker thread
        pollTimer_->setSingleShot(true);
        connect(pollTimer_, &QTimer::timeout, this, &TriageWorker::pollSignIn);
    }
    pollTimer_->start(pollIntervalMs());
}

int TriageWorker::pollIntervalMs() const {
    // qint64 arithmetic and a hard clamp: a hostile or buggy interval from the server must not overflow an int.
    const qint64 sec = qBound<qint64>(qint64(1), static_cast<qint64>(pollIntervalSec_), qint64(300));
    return static_cast<int>(sec * 1000);
}

void TriageWorker::stopPolling() {
    signingIn_ = false;
    if (pollTimer_) pollTimer_->stop();
    challenge_ = graph::DeviceCodeChallenge();
}

void TriageWorker::cancelSignIn() {
    stopPolling();
    emit signInFinished(false, QString());
}

void TriageWorker::pollSignIn() {
    if (!signingIn_) return;
    if (QDateTime::currentMSecsSinceEpoch() >= deadlineMs_) {
        stopPolling();
        emit signInFinished(false, QStringLiteral("The sign-in code expired. Start again."));
        return;
    }
    auto r = ctx_->auth()->poll(challenge_);
    if (!r.ok()) {
        if (r.error().kind == core::ErrorKind::Forbidden) {
            stopPolling();
            emit signInFinished(false, signInBlockedMessage(r.error()));
            return;
        }
        // Transient transport errors: keep polling until the code expires.
        pollTimer_->start(pollIntervalMs());
        return;
    }
    switch (r.value()) {
        case graph::PollResult::Pending:
            pollTimer_->start(pollIntervalMs());
            return;
        case graph::PollResult::SlowDown:
            pollIntervalSec_ = qBound<int>(1, pollIntervalSec_ + 5, 300);
            pollTimer_->start(pollIntervalMs());
            return;
        case graph::PollResult::Authorized: {
            stopPolling();
            const core::Status st = ctx_->complete_sign_in();
            emit signInFinished(st.ok(), st.ok() ? QString() : describeError(st.error()));
            emit errorOccurred(QString());
            emitSnapshot();
            return;
        }
        case graph::PollResult::Declined:
            stopPolling();
            emit signInFinished(false, QStringLiteral("Sign-in was declined."));
            return;
        case graph::PollResult::Expired:
            stopPolling();
            emit signInFinished(false, QStringLiteral("The sign-in code expired. Start again."));
            return;
        case graph::PollResult::Blocked:
            stopPolling();
            emit signInFinished(false, signInBlockedMessage(core::Error{}));
            return;
        case graph::PollResult::BadCode:
        case graph::PollResult::Error:
            stopPolling();
            emit signInFinished(false, QStringLiteral("Sign-in failed. Start again."));
            return;
    }
}

void TriageWorker::endSession() {
    JobGuard g(this);
    const core::Status st = ctx_->session().end();
    if (!st.ok()) emit errorOccurred(describeError(st.error()));
    emitSnapshot();
}

void TriageWorker::endSessionIfActive() {
    core::TriageSession& s = ctx_->session();
    if (s.status().session_state == core::SessionState::Active) (void)s.end();
}

void TriageWorker::signOut(bool wipe) {
    JobGuard g(this);
    stopPolling();
    const core::Status st = ctx_->sign_out(wipe);
    if (!st.ok()) {
        emit errorOccurred(describeError(st.error()));
    } else if (wipe) {
        // The store file was removed; open it again and start a fresh (signed-out) session.
        const core::Status again = ctx_->session().start();
        if (!again.ok()) emit errorOccurred(describeError(again.error()));
    }
    emitSnapshot();
}

// ---------------------------------------------------------------------------------------------------------------
// TriageController (GUI thread)
// ---------------------------------------------------------------------------------------------------------------
TriageController::TriageController(app::AppContext* ctx, QObject* parent)
    : QObject(parent),
      ctx_(ctx),
      queueModel_({"id", "subject", "fromName", "fromAddress", "receivedAt", "preview", "hasAttachments", "isRead", "flagged"}),
      activityModel_({"id", "type", "typeKey", "subject", "from", "time", "confirmation", "failureReason", "commandId",
                      "messageId", "canUndo"}),
      failedModel_({"id", "subject", "from", "action", "reason"}) {
    qRegisterMetaType<slowmail::ui::Snapshot>("slowmail::ui::Snapshot");
    if (!ctx_) return;   // demo mode
    worker_ = new TriageWorker(ctx_);
    worker_->moveToThread(&thread_);
    connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &TriageWorker::snapshotReady, this, &TriageController::onSnapshot);
    connect(worker_, &TriageWorker::jobFinished, this, &TriageController::onJobFinished);
    connect(worker_, &TriageWorker::errorOccurred, this, &TriageController::onError);
    connect(worker_, &TriageWorker::notice, this, &TriageController::onNotice);
    connect(worker_, &TriageWorker::deviceCodeReady, this, &TriageController::onDeviceCode);
    connect(worker_, &TriageWorker::signInFinished, this, &TriageController::onSignInFinished);
    connect(worker_, &TriageWorker::messageOpened, this, &TriageController::messageOpened);
    connect(worker_, &TriageWorker::messageOpenFailed, this, &TriageController::messageOpenFailed);
    thread_.setObjectName(QStringLiteral("slowmail-worker"));
    thread_.start();
}

TriageController::~TriageController() { shutdown(); }

void TriageController::shutdown() {
    if (stopped_) return;
    stopped_ = true;
    if (worker_ && thread_.isRunning()) {
        QMetaObject::invokeMethod(worker_, "endSessionIfActive", Qt::BlockingQueuedConnection);
        thread_.quit();
        thread_.wait();
    }
}

bool TriageController::refreshEnabled() const {
    return snap_.refreshEligible && snap_.signedIn && jobs_ == 0 && snap_.sessionState != QLatin1String("ended");
}

void TriageController::beginJob() {
    if (!notice_.isEmpty()) {
        notice_.clear();
        emit noticeChanged();
    }
    const bool wasIdle = (jobs_ == 0);
    ++jobs_;
    if (wasIdle) {
        emit busyChanged();
        emit statusChanged();   // refreshEnabled depends on busy
    }
}

void TriageController::onJobFinished() {
    if (jobs_ > 0) --jobs_;
    if (jobs_ == 0) {
        emit busyChanged();
        emit statusChanged();
    }
}

void TriageController::post(const char* slot, bool counted) {
    if (!worker_ || stopped_) return;
    if (counted) beginJob();
    QMetaObject::invokeMethod(worker_, slot, Qt::QueuedConnection);
}

void TriageController::postWithString(const char* slot, const QString& arg) {
    if (!worker_ || stopped_) return;
    beginJob();
    QMetaObject::invokeMethod(worker_, slot, Qt::QueuedConnection, Q_ARG(QString, arg));
}

void TriageController::postWithBool(const char* slot, bool arg) {
    if (!worker_ || stopped_) return;
    beginJob();
    QMetaObject::invokeMethod(worker_, slot, Qt::QueuedConnection, Q_ARG(bool, arg));
}

void TriageController::startSession() { post("start"); }
void TriageController::refresh() { postWithBool("refresh", false); }
void TriageController::flush() { post("flush"); }
void TriageController::retry(const QString& messageId) { postWithString("retry", messageId); }
void TriageController::openMessage(const QString& id) {
    if (demo_) {
        QTimer::singleShot(0, this, [this, id]() {
            emit messageOpened(id,
                               QStringLiteral("Hi,\n\nPlease find the revised schedule for the Q4 planning review. The "
                                              "main change is that the budget session moves to Thursday afternoon so "
                                              "that finance can attend.\n\nCould you confirm by end of day whether "
                                              "your team can present the capacity forecast? If not, we will park it "
                                              "until the January review.\n\nThanks,\nAlex"),
                               QStringLiteral("[{\"name\":\"Q4-schedule-v3.pdf\",\"type\":\"application/pdf\",\"size\":\"184.2 KB\",\"inline\":false},"
                                              "{\"name\":\"capacity-forecast.xlsx\",\"type\":\"application/vnd.ms-excel\",\"size\":\"2.4 MB\",\"inline\":false}]"),
                               false);
        });
        return;
    }
    postWithString("open", id);
}

void TriageController::archive(const QString& id) {
    queueModel_.removeById(id);   // optimistic; the next snapshot reconciles
    if (demo_) return;
    postWithString("archive", id);
}
void TriageController::remove(const QString& id) {
    queueModel_.removeById(id);
    if (demo_) return;
    postWithString("remove", id);
}
void TriageController::defer(const QString& id) {
    queueModel_.removeById(id);
    if (demo_) return;
    postWithString("defer", id);
}
void TriageController::undo(const QString& commandId) { postWithString("undo", commandId); }

void TriageController::beginSignIn() {
    userCode_.clear();
    uri_.clear();
    signInMessage_ = QStringLiteral("Contacting Microsoft...");
    emit deviceCodeChanged();
    post("beginSignIn");
}

void TriageController::cancelSignIn() {
    userCode_.clear();
    uri_.clear();
    signInMessage_.clear();
    emit deviceCodeChanged();
    post("cancelSignIn", false);
}

void TriageController::checkSignIn() { post("pollSignIn", false); }

void TriageController::endSession() { post("endSession"); }
void TriageController::signOut(bool wipe) { postWithBool("signOut", wipe); }

void TriageController::clearError() {
    dismissedError_ = sessionError_;
    localError_.clear();
    recomputeError();
}

void TriageController::recomputeError() {
    QString e = localError_;
    if (e.isEmpty() && !sessionError_.isEmpty() && sessionError_ != dismissedError_) e = sessionError_;
    if (e != lastError_) {
        lastError_ = e;
        emit lastErrorChanged();
    }
}

void TriageController::onError(const QString& message) {
    localError_ = message;
    recomputeError();
}

void TriageController::onNotice(const QString& message) {
    notice_ = message;
    emit noticeChanged();
}

void TriageController::onDeviceCode(const QString& userCode, const QString& uri) {
    userCode_ = userCode;
    uri_ = uri;
    signInMessage_ = QStringLiteral("Waiting for you to finish signing in on another device...");
    emit deviceCodeChanged();
}

void TriageController::onSignInFinished(bool ok, const QString& message) {
    userCode_.clear();
    uri_.clear();
    signInMessage_ = ok ? QString() : message;
    emit deviceCodeChanged();
    emit signInFinished(ok, message);
}

void TriageController::onSnapshot(const slowmail::ui::Snapshot& snap) {
    snap_ = snap;
    ready_ = true;
    queueModel_.setRows(snap.queue);
    activityModel_.setRows(snap.activity);
    failedModel_.setRows(snap.failed);
    if (snap.lastError != sessionError_) {
        sessionError_ = snap.lastError;
        if (sessionError_.isEmpty()) dismissedError_.clear();
    }
    recomputeError();
    emit statusChanged();
}

void TriageController::showConfigError(const QString& message) {
    localError_ = message;
    ready_ = true;
    recomputeError();
    emit statusChanged();
}

void TriageController::loadDemoData() {
    demo_ = true;
    auto row = [](const char* id, const char* subj, const char* name, const char* addr, const char* when,
                  const char* prev, bool att, bool read) {
        QVariantMap m;
        m.insert(QStringLiteral("id"), QString::fromLatin1(id));
        m.insert(QStringLiteral("subject"), QString::fromUtf8(subj));
        m.insert(QStringLiteral("fromName"), QString::fromUtf8(name));
        m.insert(QStringLiteral("fromAddress"), QString::fromLatin1(addr));
        m.insert(QStringLiteral("receivedAt"), QString::fromLatin1(when));
        m.insert(QStringLiteral("preview"), QString::fromUtf8(prev));
        m.insert(QStringLiteral("hasAttachments"), att);
        m.insert(QStringLiteral("isRead"), read);
        m.insert(QStringLiteral("flagged"), false);
        return m;
    };
    snap_.sessionState = QStringLiteral("active");
    snap_.downloadedAt = QStringLiteral("01:42");
    snap_.refreshEligible = false;
    snap_.nextEligibleAt = QStringLiteral("05:42");
    snap_.pendingCount = 1;
    snap_.deferredCount = 2;
    snap_.failedCount = 1;
    snap_.signedIn = true;
    userCode_ = QStringLiteral("B7QX-4MZ9");
    uri_ = QStringLiteral("https://microsoft.com/devicelogin");
    signInMessage_ = QStringLiteral("Waiting for you to finish signing in on another device...");
    snap_.queue = {
        row("d1", "Q4 planning review: revised schedule and a deliberately long subject line that must wrap onto a second line and then be elided", "Alex Morgan", "alex@example.com", "00:58", "Please find the revised schedule attached. The budget session moves to Thursday.", true, false),
        row("d2", "Invoice 2026-1043 overdue", "Accounts Payable", "ap@example.com", "Mon 23:10", "This is a reminder that invoice 2026-1043 is now 14 days overdue.", true, true),
        row("d3", "Can you review the draft?", "Priya Shah", "priya@example.com", "23:02", "Hi, when you get a moment could you look over the draft policy?", false, false),
        row("d4", "Lunch on Friday", "Sam Lee", "sam@example.com", "22:47", "Are you free for lunch on Friday? There is a new place near the office.", false, true),
        row("d5", "Security notice: new sign-in", "Microsoft Account Team", "account@example.com", "21:15", "We noticed a new sign-in to your account from a new device.", false, true),
        row("d6", "Re: Vendor contract renewal", "Jordan Wu", "jordan@example.com", "20:30", "Thanks for the update. Legal has approved the redlines.", true, true),
        row("d7", "Weekly digest", "Team Updates", "updates@example.com", "19:00", "Here is what happened across the team this week.", false, true),
    };
    queueModel_.setRows(snap_.queue);
    auto act = [](const char* id, const char* type, const char* key, const char* subj, const char* from, const char* t,
                  const char* conf, const char* reason, bool undo) {
        QVariantMap m;
        m.insert(QStringLiteral("id"), QString::fromLatin1(id));
        m.insert(QStringLiteral("type"), QString::fromLatin1(type));
        m.insert(QStringLiteral("typeKey"), QString::fromLatin1(key));
        m.insert(QStringLiteral("subject"), QString::fromUtf8(subj));
        m.insert(QStringLiteral("from"), QString::fromUtf8(from));
        m.insert(QStringLiteral("time"), QString::fromLatin1(t));
        m.insert(QStringLiteral("confirmation"), QString::fromLatin1(conf));
        m.insert(QStringLiteral("failureReason"), QString::fromUtf8(reason));
        m.insert(QStringLiteral("commandId"), QString::fromLatin1(id));
        m.insert(QStringLiteral("messageId"), QString::fromLatin1(id));
        m.insert(QStringLiteral("canUndo"), undo);
        return m;
    };
    snap_.activity = {
        act("a1", "Archived", "archived", "Board minutes", "Chris Dale", "01:50", "Confirmed", "", true),
        act("a2", "Archived", "archived", "Parking permit", "Facilities", "01:48", "Pending", "", false),
        act("a3", "Deleted", "deleted", "You won a prize", "Promo Bot", "01:45", "Confirmed", "", true),
        act("a4", "Deferred", "deferred", "Budget questions", "Finance", "01:44", "Failed", "The message no longer exists on the server.", false),
    };
    activityModel_.setRows(snap_.activity);
    {
        QVariantMap f;
        f.insert(QStringLiteral("id"), QStringLiteral("f1"));
        f.insert(QStringLiteral("subject"), QStringLiteral("Budget questions"));
        f.insert(QStringLiteral("from"), QStringLiteral("Finance"));
        f.insert(QStringLiteral("action"), QStringLiteral("Defer"));
        f.insert(QStringLiteral("reason"), QStringLiteral("ErrorFolderNotFound"));
        snap_.failed = {f};
        failedModel_.setRows(snap_.failed);
    }
    ready_ = true;
    emit statusChanged();
}

} // namespace slowmail::ui
