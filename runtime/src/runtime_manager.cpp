#include <runtime/runtime_manager.h>

#include "sqlite_db.h"

#include <ctime>

namespace fs = std::filesystem;
using namespace aow::sql;

namespace runtime {
namespace {

constexpr int kSchemaVersion = 1;
constexpr std::size_t kHistoryKept = 1000;

constexpr const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS sessions (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    package_name  TEXT NOT NULL,
    label         TEXT NOT NULL,
    version_code  INTEGER NOT NULL,
    backend       TEXT NOT NULL,
    device        TEXT NOT NULL,
    pid           INTEGER NOT NULL,
    process_token TEXT NOT NULL,
    state         TEXT NOT NULL,     -- running | exited | crashed | stopped | failed
    started_at    INTEGER NOT NULL,
    ended_at      INTEGER NOT NULL,  -- 0 while running
    exit_code     INTEGER,           -- NULL if unknown
    detail        TEXT NOT NULL
);
-- At most one running instance of an app per runtime, even across processes.
CREATE UNIQUE INDEX IF NOT EXISTS one_running_instance
    ON sessions (package_name, backend, device) WHERE state = 'running';
)sql";

constexpr const char* kColumns =
    "SELECT id, package_name, label, version_code, backend, device, pid, process_token, state,"
    " started_at, ended_at, exit_code, detail FROM sessions";

std::int64_t now() { return std::int64_t(std::time(nullptr)); }

const char* stateName(AppState s) {
    switch (s) {
    case AppState::Running: return "running";
    case AppState::Exited: return "exited";
    case AppState::Crashed: return "crashed";
    case AppState::Stopped: return "stopped";
    case AppState::Failed: return "failed";
    }
    return "failed";
}

AppState parseState(const std::string& s) {
    for (AppState st : {AppState::Running, AppState::Exited, AppState::Crashed, AppState::Stopped, AppState::Failed})
        if (s == stateName(st)) return st;
    return AppState::Failed;
}

} // namespace

const char* toString(AppState s) { return stateName(s); }

struct RuntimeManager::Db {
    explicit Db(const fs::path& file) : db(file) {}
    Database db;
};

RuntimeManager::RuntimeManager(pkgmgr::PackageManager& pm, std::unique_ptr<RuntimeBackend> backend)
    : pm_(pm), backend_(std::move(backend)) {
    if (!backend_) throw RuntimeError("no runtime backend");
    try {
        db_ = std::make_unique<Db>(pm_.root() / "runtime.db");
        db_->db.exec("PRAGMA journal_mode=WAL;");
        auto version = db_->db.prepare("PRAGMA user_version");
        version.step();
        if (version.int64(0) > kSchemaVersion) throw RuntimeError("runtime.db was created by a newer version");
        db_->db.exec(kSchema);
        db_->db.exec(("PRAGMA user_version=" + std::to_string(kSchemaVersion)).c_str());
        db_->db.prepare("DELETE FROM sessions WHERE state != 'running' AND id <= "
                        "(SELECT MAX(id) FROM sessions) - ?1")
            .bind(1, std::int64_t(kHistoryKept))
            .run();
    } catch (const DbError& e) {
        throw RuntimeError(std::string("runtime database: ") + e.what());
    }
}

RuntimeManager::~RuntimeManager() { stopMonitor(); }

std::vector<AppSession> RuntimeManager::querySessions(const std::string& where, const std::string& arg,
                                                      std::size_t limit) const {
    // `where` may use ?1 = backend, ?2 = device, ?3 = arg.
    const std::string sql = std::string(kColumns) + " WHERE " + where + " ORDER BY id DESC LIMIT " + std::to_string(limit);
    auto q = db_->db.prepare(sql.c_str());
    const int params = q.parameterCount();
    if (params >= 1) q.bind(1, backend_->name());
    if (params >= 2) q.bind(2, backend_->device());
    if (params >= 3) q.bind(3, arg);
    std::vector<AppSession> out;
    while (q.step()) {
        AppSession s;
        s.id = q.int64(0);
        s.packageName = q.text(1);
        s.label = q.text(2);
        s.versionCode = q.int64(3);
        s.backend = q.text(4);
        s.device = q.text(5);
        s.pid = q.int64(6);
        s.processToken = q.text(7);
        s.state = parseState(q.text(8));
        s.startedAt = q.int64(9);
        s.endedAt = q.int64(10);
        if (!q.isNull(11)) s.exitCode = int(q.int64(11));
        s.detail = q.text(12);
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<RuntimeEvent> RuntimeManager::refreshLocked(const std::string& onlyPackage) {
    std::vector<RuntimeEvent> events;
    const std::string where = std::string("state = 'running' AND backend = ?1 AND device = ?2") +
                              (onlyPackage.empty() ? "" : " AND package_name = ?3");
    for (AppSession s : querySessions(where, onlyPackage, 1000)) {
        const Liveness l = backend_->check(s);
        if (l.alive) continue;
        s.state = l.exitCode && *l.exitCode != 0 ? AppState::Crashed : AppState::Exited;
        s.endedAt = now();
        s.exitCode = l.exitCode;
        if (!l.detail.empty()) s.detail = l.detail;
        auto u = db_->db.prepare("UPDATE sessions SET state = ?1, ended_at = ?2, exit_code = ?3, detail = ?4 "
                                 "WHERE id = ?5 AND state = 'running'");
        u.bind(1, std::string(stateName(s.state))).bind(2, s.endedAt);
        if (s.exitCode) u.bind(3, std::int64_t(*s.exitCode));
        u.bind(4, s.detail).bind(5, s.id).run();
        events.push_back({s.state == AppState::Crashed ? EventKind::Crashed : EventKind::Exited, s});
        known_.erase(s.id);
    }
    return events;
}

// Turns changes made by other processes (a CLI launch/stop, another launcher)
// into events, by diffing the running set against the previous one.
void RuntimeManager::reconcileLocked(std::vector<RuntimeEvent>& events) {
    std::set<std::int64_t> reported;
    for (const auto& e : events) reported.insert(e.session.id);

    std::set<std::int64_t> now;
    for (const auto& s : querySessions("state = 'running' AND backend = ?1 AND device = ?2", {}, 1000)) {
        now.insert(s.id);
        if (knownInit_ && !known_.count(s.id) && !reported.count(s.id)) events.push_back({EventKind::Started, s});
    }
    for (const std::int64_t id : known_) {
        if (now.count(id) || reported.count(id)) continue;
        const auto ended = querySessions("id = ?3", std::to_string(id), 1);
        if (ended.empty()) continue;
        const AppSession& s = ended.front();
        const EventKind kind = s.state == AppState::Stopped   ? EventKind::Stopped
                               : s.state == AppState::Crashed ? EventKind::Crashed
                                                              : EventKind::Exited;
        events.push_back({kind, s});
    }
    known_ = std::move(now);
    knownInit_ = true;
}

void RuntimeManager::refresh() {
    std::vector<RuntimeEvent> events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        try {
            events = refreshLocked({});
            reconcileLocked(events);
        } catch (const DbError&) {}
    }
    emit(events);
}

LaunchResult RuntimeManager::launch(const std::string& packageName) {
    LaunchResult res;
    std::vector<RuntimeEvent> events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        try {
            events = refreshLocked(packageName);
            const auto current =
                querySessions("state = 'running' AND backend = ?1 AND device = ?2 AND package_name = ?3", packageName, 1);
            if (!current.empty()) {
                res.ok = res.alreadyRunning = true;
                res.session = current.front();
            } else if (const auto pkg = pm_.find(packageName); !pkg) {
                res.error = packageName + " is not installed";
            } else {
                AppSession s;
                s.packageName = pkg->packageName;
                s.label = pkg->displayName();
                s.versionCode = pkg->versionCode;
                s.backend = backend_->name();
                s.device = backend_->device();
                s.startedAt = now();

                RuntimeBackend::Process proc;
                std::string error;
                const bool started = backend_->launch(*pkg, proc, error);
                s.pid = proc.pid;
                s.processToken = proc.token;
                s.state = started ? AppState::Running : AppState::Failed;
                s.endedAt = started ? 0 : now();
                s.detail = error;

                auto ins = db_->db.prepare(
                    "INSERT INTO sessions (package_name, label, version_code, backend, device, pid, process_token,"
                    " state, started_at, ended_at, detail) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)");
                ins.bind(1, s.packageName).bind(2, s.label).bind(3, s.versionCode).bind(4, s.backend)
                    .bind(5, s.device).bind(6, s.pid).bind(7, s.processToken)
                    .bind(8, std::string(stateName(s.state))).bind(9, s.startedAt).bind(10, s.endedAt)
                    .bind(11, s.detail);
                try {
                    ins.run();
                } catch (const DbError&) {
                    // Another process launched the same app at the same moment - keep theirs.
                    std::string ignored;
                    if (started) backend_->stop(s, ignored);
                    throw;
                }
                auto id = db_->db.prepare("SELECT last_insert_rowid()");
                id.step();
                s.id = id.int64(0);

                res.ok = started;
                res.error = error;
                res.session = s;
                if (started) {
                    events.push_back({EventKind::Started, s});
                    known_.insert(s.id);
                }
            }
        } catch (const DbError& e) {
            res.ok = false;
            res.error = std::string("runtime database: ") + e.what();
        }
    }
    emit(events);
    return res;
}

bool RuntimeManager::stop(const std::string& packageName, std::string* error) {
    auto setError = [&](std::string msg) { if (error) *error = std::move(msg); };
    std::vector<RuntimeEvent> events;
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        try {
            events = refreshLocked(packageName);
            auto current =
                querySessions("state = 'running' AND backend = ?1 AND device = ?2 AND package_name = ?3", packageName, 1);
            if (current.empty()) {
                setError(packageName + " is not running");
            } else {
                AppSession s = current.front();
                std::string err;
                if (!backend_->stop(s, err)) {
                    setError(err);
                } else {
                    s.state = AppState::Stopped;
                    s.endedAt = now();
                    db_->db.prepare("UPDATE sessions SET state = 'stopped', ended_at = ?1 WHERE id = ?2")
                        .bind(1, s.endedAt).bind(2, s.id).run();
                    events.push_back({EventKind::Stopped, s});
                    known_.erase(s.id);
                    ok = true;
                }
            }
        } catch (const DbError& e) {
            setError(std::string("runtime database: ") + e.what());
        }
    }
    emit(events);
    return ok;
}

std::optional<AppSession> RuntimeManager::running(const std::string& packageName) const {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        auto s = querySessions("state = 'running' AND backend = ?1 AND device = ?2 AND package_name = ?3", packageName, 1);
        if (!s.empty()) return s.front();
    } catch (const DbError&) {}
    return std::nullopt;
}

std::vector<AppSession> RuntimeManager::running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        return querySessions("state = 'running' AND backend = ?1 AND device = ?2", {}, 1000);
    } catch (const DbError&) {
        return {};
    }
}

std::vector<AppSession> RuntimeManager::history(std::size_t limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        return querySessions("1", {}, limit);
    } catch (const DbError&) {
        return {};
    }
}

void RuntimeManager::setListener(Listener listener) {
    std::lock_guard<std::mutex> lock(listenerMutex_);
    listener_ = std::move(listener);
}

void RuntimeManager::emit(const std::vector<RuntimeEvent>& events) {
    if (events.empty()) return;
    Listener l;
    {
        std::lock_guard<std::mutex> lock(listenerMutex_);
        l = listener_;
    }
    if (l)
        for (const auto& e : events) l(e);
}

void RuntimeManager::startMonitor(std::chrono::milliseconds interval) {
    stopMonitor();
    {
        std::lock_guard<std::mutex> lock(monitorMutex_);
        monitorStop_ = false;
    }
    monitor_ = std::thread([this, interval] {
        std::unique_lock<std::mutex> lock(monitorMutex_);
        while (!monitorStop_) {
            lock.unlock();
            refresh();
            lock.lock();
            monitorWake_.wait_for(lock, interval, [this] { return monitorStop_; });
        }
    });
}

void RuntimeManager::stopMonitor() {
    {
        std::lock_guard<std::mutex> lock(monitorMutex_);
        monitorStop_ = true;
    }
    monitorWake_.notify_all();
    if (monitor_.joinable()) monitor_.join();
}

} // namespace runtime
