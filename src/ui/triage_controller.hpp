// Qt glue between QML and core::TriageSession.
//
// Threading model: TriageSession is single-threaded, so EVERY call into it (including the cheap local ones) runs
// on one worker QThread, serialised by that thread's event loop. The GUI thread never touches the session; it only
// posts jobs and receives immutable Snapshot values back. No tokens or message content are ever logged here.
#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QObject>
#include <QString>
#include <QThread>
#include <QTimer>
#include <QVariantMap>
#include <QVector>

#include <memory>

#include "app_context.hpp"
#include "slowmail/graph/auth.hpp"

namespace slowmail::ui {

// Everything the GUI needs after any job. Plain value type, copied across threads.
struct Snapshot {
    QString sessionState = QStringLiteral("not_started");
    QString downloadedAt = QStringLiteral("never");
    bool refreshEligible = false;
    QString nextEligibleAt;
    int pendingCount = 0;
    int deferredCount = 0;
    bool offline = false;
    bool signedIn = false;
    QString lastError;
    QVector<QVariantMap> queue;
    QVector<QVariantMap> activity;
};

// Minimal keyed list model over QVariantMap rows. Updates are diffed on the "id" key so that a ListView keeps its
// position (no jump to the top) after an action.
class RowModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
public:
    RowModel(const QList<QByteArray>& roles, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return rows_.size(); }
    void setRows(const QVector<QVariantMap>& rows);
    void removeById(const QString& id);
    Q_INVOKABLE QVariantMap get(int row) const;

signals:
    void countChanged();

private:
    QList<QByteArray> roles_;
    QVector<QVariantMap> rows_;
};

// Lives on the worker thread.
class TriageWorker : public QObject {
    Q_OBJECT
public:
    explicit TriageWorker(app::AppContext* ctx, QObject* parent = nullptr);

public slots:
    void start();
    void refresh(bool force);
    void flush();
    void open(const QString& id);
    void archive(const QString& id);
    void remove(const QString& id);
    void defer(const QString& id);
    void undefer(const QString& id);
    void undo(const QString& commandId);
    void beginSignIn();
    void cancelSignIn();
    void pollSignIn();
    void endSession();
    void endSessionIfActive();   // used on application exit
    void signOut(bool wipe);
    void resnapshot();

signals:
    void snapshotReady(const slowmail::ui::Snapshot& snap);
    void messageOpened(const QString& id, const QString& text, const QString& attachmentsJson, bool truncated);
    void messageOpenFailed(const QString& id, const QString& reason);
    void deviceCodeReady(const QString& userCode, const QString& uri);
    void signInFinished(bool ok, const QString& message);
    void errorOccurred(const QString& message);
    void notice(const QString& message);
    void jobFinished();

private:
    Snapshot makeSnapshot();
    void emitSnapshot();
    void stopPolling();
    void actionResult(const core::Outcome<core::Command>& r);

    app::AppContext* ctx_;
    QTimer* pollTimer_ = nullptr;
    bool signingIn_ = false;
    graph::DeviceCodeChallenge challenge_;   // holds the secret device_code: never exposed, never logged
    int pollIntervalSec_ = 5;
    qint64 deadlineMs_ = 0;
};

class TriageController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool ready READ ready NOTIFY statusChanged)
    Q_PROPERTY(QString sessionState READ sessionState NOTIFY statusChanged)
    Q_PROPERTY(QString downloadedAt READ downloadedAt NOTIFY statusChanged)
    Q_PROPERTY(bool refreshEnabled READ refreshEnabled NOTIFY statusChanged)
    Q_PROPERTY(QString nextEligibleAt READ nextEligibleAt NOTIFY statusChanged)
    Q_PROPERTY(int pendingCount READ pendingCount NOTIFY statusChanged)
    Q_PROPERTY(int deferredCount READ deferredCount NOTIFY statusChanged)
    Q_PROPERTY(bool offline READ offline NOTIFY statusChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(bool signedIn READ signedIn NOTIFY statusChanged)
    Q_PROPERTY(QString deviceCodeUserCode READ deviceCodeUserCode NOTIFY deviceCodeChanged)
    Q_PROPERTY(QString deviceCodeUri READ deviceCodeUri NOTIFY deviceCodeChanged)
    Q_PROPERTY(QString signInMessage READ signInMessage NOTIFY deviceCodeChanged)
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)
    Q_PROPERTY(QObject* queueModel READ queueModel CONSTANT)
    Q_PROPERTY(QObject* activityModel READ activityModel CONSTANT)

public:
    // `ctx` may be null only in demo mode (see loadDemoData()).
    explicit TriageController(app::AppContext* ctx, QObject* parent = nullptr);
    ~TriageController() override;

    bool ready() const { return ready_; }
    QString sessionState() const { return snap_.sessionState; }
    QString downloadedAt() const { return snap_.downloadedAt; }
    bool refreshEnabled() const;
    QString nextEligibleAt() const { return snap_.nextEligibleAt; }
    int pendingCount() const { return snap_.pendingCount; }
    int deferredCount() const { return snap_.deferredCount; }
    bool offline() const { return snap_.offline; }
    bool busy() const { return jobs_ > 0; }
    QString lastError() const { return lastError_; }
    bool signedIn() const { return snap_.signedIn; }
    QString deviceCodeUserCode() const { return userCode_; }
    QString deviceCodeUri() const { return uri_; }
    QString signInMessage() const { return signInMessage_; }
    QString notice() const { return notice_; }
    QObject* queueModel() { return &queueModel_; }
    QObject* activityModel() { return &activityModel_; }

    // Opens the store and starts/resumes the session (worker thread). Never touches the network.
    Q_INVOKABLE void startSession();
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void flush();
    Q_INVOKABLE void openMessage(const QString& id);
    Q_INVOKABLE void archive(const QString& id);
    Q_INVOKABLE void remove(const QString& id);
    Q_INVOKABLE void defer(const QString& id);
    Q_INVOKABLE void undo(const QString& commandId);
    Q_INVOKABLE void beginSignIn();
    Q_INVOKABLE void cancelSignIn();
    Q_INVOKABLE void endSession();
    Q_INVOKABLE void signOut(bool wipe);
    Q_INVOKABLE void clearError();

    // Ends an active session (flushing pending commands) and stops the worker. Blocks; call on exit only.
    void shutdown();

    // Populate the models and a canned message body without a session. Used only by `--demo` for UI review
    // screenshots; nothing is written anywhere.
    void loadDemoData();

signals:
    void statusChanged();
    void busyChanged();
    void lastErrorChanged();
    void noticeChanged();
    void deviceCodeChanged();
    void messageOpened(const QString& id, const QString& text, const QString& attachmentsJson, bool truncated);
    void messageOpenFailed(const QString& id, const QString& reason);
    void signInFinished(bool ok, const QString& message);

private slots:
    void onSnapshot(const slowmail::ui::Snapshot& snap);
    void onJobFinished();
    void onError(const QString& message);
    void onNotice(const QString& message);
    void onDeviceCode(const QString& userCode, const QString& uri);
    void onSignInFinished(bool ok, const QString& message);

private:
    void post(const char* slot, bool counted = true);
    void postWithString(const char* slot, const QString& arg);
    void postWithBool(const char* slot, bool arg);
    void beginJob();
    void recomputeError();

    app::AppContext* ctx_;
    QThread thread_;
    TriageWorker* worker_ = nullptr;
    RowModel queueModel_;
    RowModel activityModel_;
    Snapshot snap_;
    bool ready_ = false;
    bool demo_ = false;
    int jobs_ = 0;
    bool stopped_ = false;
    QString lastError_;      // what the banner shows: localError_ if any, else an un-dismissed sessionError_
    QString localError_;     // from worker jobs (cleared by the next successful job or by dismissal)
    QString sessionError_;   // StatusSnapshot::last_error (already redacted by the core)
    QString dismissedError_;
    QString notice_;
    QString userCode_;
    QString uri_;
    QString signInMessage_;
};

} // namespace slowmail::ui

Q_DECLARE_METATYPE(slowmail::ui::Snapshot)
