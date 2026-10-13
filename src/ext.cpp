// bbcenter_ext.dll — BBCenter（贝塔投屏中控）保活增强 PoC v0.1
// 不修改目标程序字节：仅进程内 hook + 调用 Qt 公开 API。
// 挂载点：① 右键设备菜单追加「保活锁定」 ② 列表真列「保活锁定」 ③ 主线程看门狗
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <mutex>
#include <vector>

#include <QApplication>
#include <QCoreApplication>
#include <QWidget>
#include <QAbstractButton>
#include <QTableView>
#include <QAbstractItemView>
#include <QAbstractItemModel>
#include <QIdentityProxyModel>
#include <QMenu>
#include <QAction>
#include <QTimer>
#include <QMetaObject>
#include <QString>
#include <QStringList>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QSettings>
#include <QDateTime>
#include <QHash>
#include <QVariant>

// ---------------------------------------------------------------- 基础

static HINSTANCE g_self = nullptr;
static std::wstring g_dir;                 // DLL 所在目录
static std::mutex   g_logMx;
static FILE*        g_log = nullptr;
static volatile LONG g_setupDone = 0;

static void logf(const char* fmt, ...) {
    char ts[32]; SYSTEMTIME st; GetLocalTime(&st);
    sprintf(ts, "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    std::lock_guard<std::mutex> lk(g_logMx);
    if (!g_log) return;
    fprintf(g_log, "[%s] ", ts);
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
}

static std::wstring fileInDir(const wchar_t* name) {
    std::wstring p = g_dir; p += L"\\"; p += name; return p;
}
static QString qFileInDir(const wchar_t* name) {
    return QString::fromStdWString(fileInDir(name));
}

// ---------------------------------------------------------------- 配置

struct Config {
    bool enabled = true;
    bool dryRun = true;
    bool enableColumn = false;
    bool useCheck = true;
    int  sweepMs = 10000;
    int  cooldownMs = 90000;
    int  maxRetry = 5;
    QString route = QStringLiteral("p1");
} g_cfg;

static void loadConfig() {
    const QString ini = qFileInDir(L"bbcenter_ext.ini");
    if (!QFile::exists(ini)) {
        QFile f(ini);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            f.write("[ext]\n"
                    "enabled=1\n"
                    "dry_run=1\n"
                    "enable_column=0\n"
                    "sweep_ms=10000\n"
                    "cooldown_ms=90000\n"
                    "max_retry=5\n"
                    "use_check=1\n"
                    "trigger_route=p1\n");
            f.close();
        }
    }
    QSettings s(ini, QSettings::IniFormat);
    g_cfg.enabled      = s.value("ext/enabled", 1).toInt() != 0;
    g_cfg.dryRun       = s.value("ext/dry_run", 1).toInt() != 0;
    g_cfg.enableColumn = s.value("ext/enable_column", 0).toInt() != 0;
    g_cfg.useCheck     = s.value("ext/use_check", 1).toInt() != 0;
    g_cfg.sweepMs      = s.value("ext/sweep_ms", 10000).toInt();
    g_cfg.cooldownMs   = s.value("ext/cooldown_ms", 90000).toInt();
    g_cfg.maxRetry     = s.value("ext/max_retry", 5).toInt();
    g_cfg.route        = s.value("ext/trigger_route", "p1").toString();
    logf("[cfg] enabled=%d dry_run=%d enable_column=%d sweep=%dms cooldown=%dms max_retry=%d route=%s",
         (int)g_cfg.enabled, (int)g_cfg.dryRun, (int)g_cfg.enableColumn,
         g_cfg.sweepMs, g_cfg.cooldownMs, g_cfg.maxRetry, g_cfg.route.toUtf8().constData());
}

// ---------------------------------------------------------------- 锁定状态

struct DevState {
    bool   locked = false;
    bool   bad = false;
    qint64 lastTrigger = 0;
    int    retries = 0;
    QString alias;
};
static QHash<QString, DevState> g_dev;
static std::mutex g_devMx;

static void saveState() {
    std::lock_guard<std::mutex> lk(g_devMx);
    QJsonObject root, devs;
    root["ver"] = 1;
    for (auto it = g_dev.begin(); it != g_dev.end(); ++it) {
        QJsonObject o;
        o["locked"] = it.value().locked;
        o["bad"] = it.value().bad;
        o["retries"] = it.value().retries;
        o["lastTrigger"] = double(it.value().lastTrigger);
        if (!it.value().alias.isEmpty()) o["alias"] = it.value().alias;
        devs[it.key()] = o;
    }
    root["devices"] = devs;
    const QString tmp = qFileInDir(L"bbcenter_ext.json.tmp");
    const QString dst = qFileInDir(L"bbcenter_ext.json");
    QFile f(tmp);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    f.close();
    MoveFileExW(tmp.toStdWString().c_str(), dst.toStdWString().c_str(), MOVEFILE_REPLACE_EXISTING);
}

static void loadState() {
    QFile f(qFileInDir(L"bbcenter_ext.json"));
    if (!f.open(QIODevice::ReadOnly)) return;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    f.close();
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        logf("[state] json parse failed: %s", err.errorString().toUtf8().constData());
        return;
    }
    const QJsonObject devs = doc.object().value("devices").toObject();
    std::lock_guard<std::mutex> lk(g_devMx);
    for (auto it = devs.begin(); it != devs.end(); ++it) {
        DevState st;
        const QJsonObject o = it.value().toObject();
        st.locked = o.value("locked").toBool();
        st.bad = o.value("bad").toBool();
        st.retries = o.value("retries").toInt();
        st.lastTrigger = qint64(o.value("lastTrigger").toDouble());
        st.alias = o.value("alias").toString();
        g_dev[it.key()] = st;
    }
    logf("[state] loaded %d device(s)", (int)g_dev.size());
}

// ---------------------------------------------------------------- 控件定位

static QWidget* findByName(const QString& name) {
    const QWidgetList ws = QApplication::allWidgets();
    for (QWidget* w : ws) if (w->objectName() == name) return w;
    return nullptr;
}

static QTableView* g_view = nullptr;
static QWidget* g_win = nullptr;

static void locateWidgets() {
    if (!g_view) {
        if (QWidget* w = findByName(QStringLiteral("deviceView")))
            g_view = qobject_cast<QTableView*>(w);
        if (g_view) logf("[init] found deviceView=%p model=%p", (void*)g_view,
                         (void*)g_view->model());
    }
    if (!g_win) {
        const QWidgetList tops = QApplication::topLevelWidgets();
        for (QWidget* w : tops) {
            if (w->findChild<QTableView*>(QStringLiteral("deviceView"))) { g_win = w; break; }
        }
        if (g_win) logf("[init] mainWindow ok name='%s' title='%s'",
                        g_win->objectName().toUtf8().constData(),
                        g_win->windowTitle().toUtf8().constData());
    }
}

// 列索引按“列标题文本”定位，厂商加列/换序也不炸
struct Cols { int id = -1, run = -1, conn = -1, auth = -1, name = -1, remark = -1; };

static Cols locateColumns(QAbstractItemModel* m) {
    Cols c;
    if (!m) return c;
    const int n = m->columnCount();
    for (int i = 0; i < n; ++i) {
        const QString h = m->headerData(i, Qt::Horizontal, Qt::DisplayRole).toString().trimmed();
        if (h == QStringLiteral("设备号")) c.id = i;
        else if (h == QStringLiteral("运行")) c.run = i;
        else if (h == QStringLiteral("连接")) c.conn = i;
        else if (h == QStringLiteral("授权")) c.auth = i;
        else if (h == QStringLiteral("设备名称")) c.name = i;
        else if (h == QStringLiteral("备注")) c.remark = i;
    }
    return c;
}

static QString cellText(QAbstractItemModel* m, int row, int col) {
    if (!m || col < 0 || row < 0 || row >= m->rowCount()) return QString();
    return m->data(m->index(row, col), Qt::DisplayRole).toString().trimmed();
}

static int rowOfDevId(QAbstractItemModel* m, int idCol, const QString& id) {
    if (!m || idCol < 0) return -1;
    for (int r = 0, n = m->rowCount(); r < n; ++r)
        if (cellText(m, r, idCol) == id) return r;
    return -1;
}

static void dumpList(const char* why) {
    if (!g_view) return;
    QAbstractItemModel* m = g_view->model();
    if (!m) { logf("[dump:%s] no model", why); return; }
    const Cols c = locateColumns(m);
    logf("[dump:%s] model=%p class='%s' rows=%d cols=%d", why, (void*)m,
         m->metaObject()->className(), m->rowCount(), m->columnCount());
    QStringList hs;
    for (int i = 0; i < m->columnCount(); ++i)
        hs << m->headerData(i, Qt::Horizontal, Qt::DisplayRole).toString();
    logf("[dump:%s] header = %s", why, hs.join(QStringLiteral(" | ")).toUtf8().constData());
    logf("[dump:%s] colIdx id=%d run=%d conn=%d auth=%d name=%d remark=%d", why,
         c.id, c.run, c.conn, c.auth, c.name, c.remark);
    const int maxRow = qMin(m->rowCount(), 5);
    for (int r = 0; r < maxRow; ++r) {
        QStringList vs;
        for (int i = 0; i < m->columnCount(); ++i) vs << cellText(m, r, i);
        logf("[dump:%s] row%d = %s", why, r, vs.join(QStringLiteral(" | ")).toUtf8().constData());
    }
}

// ---------------------------------------------------------------- 真列（代理模型）

static Cols g_cols;

class LockProxy : public QIdentityProxyModel {
public:
    explicit LockProxy(QObject* parent = nullptr) : QIdentityProxyModel(parent) {}

    int extraCol() const { return QIdentityProxyModel::columnCount(); }

    int columnCount(const QModelIndex& parent = QModelIndex()) const override {
        if (parent.isValid()) return QIdentityProxyModel::columnCount(parent);
        return QIdentityProxyModel::columnCount(parent) + 1;
    }
    QVariant headerData(int section, Qt::Orientation o, int role) const override {
        if (o == Qt::Horizontal && section == extraCol())
            return role == Qt::DisplayRole ? QStringLiteral("保活锁定") : QVariant();
        return QIdentityProxyModel::headerData(section, o, role);
    }
    QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override {
        if (parent.isValid()) return QModelIndex();
        if (column == extraCol()) return createIndex(row, column);
        return QIdentityProxyModel::index(row, column, parent);
    }
    QModelIndex parent(const QModelIndex&) const override { return QModelIndex(); }
    QModelIndex mapToSource(const QModelIndex& p) const override {
        if (p.isValid() && p.column() == extraCol()) return QModelIndex();
        return QIdentityProxyModel::mapToSource(p);
    }
    QVariant data(const QModelIndex& idx, int role) const override {
        if (idx.isValid() && idx.column() == extraCol()) {
            const QString id = cellText(sourceModel(), idx.row(), g_cols.id);
            bool locked = false, bad = false;
            { std::lock_guard<std::mutex> lk(g_devMx);
              auto it = g_dev.constFind(id);
              if (it != g_dev.constEnd()) { locked = it->locked; bad = it->bad; } }
            if (role == Qt::DisplayRole)
                return bad ? QStringLiteral("❗ 异常停手")
                           : (locked ? QStringLiteral("🔒 已锁定") : QStringLiteral("— 未锁定"));
            if (role == Qt::TextAlignmentRole) return int(Qt::AlignCenter);
            return QVariant();
        }
        return QIdentityProxyModel::data(idx, role);
    }
    Qt::ItemFlags flags(const QModelIndex& idx) const override {
        if (idx.isValid() && idx.column() == extraCol())
            return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        return QIdentityProxyModel::flags(idx);
    }
};

static LockProxy* g_proxy = nullptr;
static QAbstractItemModel* g_srcModel = nullptr;

static void installColumn() {
    if (!g_view) return;
    QAbstractItemModel* src = g_view->model();
    if (!src) return;
    if (dynamic_cast<LockProxy*>(src)) return;                 // 已是我们的
    g_srcModel = src;
    g_proxy = new LockProxy(g_view);
    g_proxy->setSourceModel(src);
    logf("[column] swapping model %p -> proxy %p (src rows=%d cols=%d)",
         (void*)src, (void*)g_proxy, src->rowCount(), src->columnCount());
    g_view->setModel(g_proxy);
    g_view->setColumnWidth(g_proxy->extraCol(), 96);
    if (g_view->model() != g_proxy) {
        logf("[column] !! swap failed, restoring original model");
        g_view->setModel(src);
        delete g_proxy; g_proxy = nullptr;
    } else {
        logf("[column] ok, extra col = %d", g_proxy->extraCol());
    }
}

// ---------------------------------------------------------------- 右键菜单 hook（IAT patch）

static void* patchIat(HMODULE mod, const char* dllName, const char* fnName, void* newFn) {
    if (!mod) return nullptr;
    auto dos = (PIMAGE_DOS_HEADER)mod;
    auto nt = (PIMAGE_NT_HEADERS)((BYTE*)mod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    const auto& imp = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imp.VirtualAddress) return nullptr;
    auto desc = (PIMAGE_IMPORT_DESCRIPTOR)((BYTE*)mod + imp.VirtualAddress);
    for (; desc->Name; ++desc) {
        const char* name = (const char*)((BYTE*)mod + desc->Name);
        if (_stricmp(name, dllName) != 0) continue;
        DWORD oftRva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
        auto oft = (PIMAGE_THUNK_DATA)((BYTE*)mod + oftRva);
        auto ft = (PIMAGE_THUNK_DATA)((BYTE*)mod + desc->FirstThunk);
        for (; oft->u1.AddressOfData; ++oft, ++ft) {
            if (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            auto ibn = (PIMAGE_IMPORT_BY_NAME)((BYTE*)mod + oft->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, fnName) != 0) continue;
            void* orig = (void*)ft->u1.Function;
            DWORD old = 0;
            if (VirtualProtect(&ft->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
                ft->u1.Function = (ULONG_PTR)newFn;
                VirtualProtect(&ft->u1.Function, sizeof(void*), old, &old);
            }
            return orig;
        }
    }
    return nullptr;
}

typedef QAction* (*ExecFn)(QMenu*, const QPoint&, QAction*);
static ExecFn g_origExec = nullptr;

static QString currentDevId() {
    if (!g_view) return QString();
    QAbstractItemModel* m = g_view->model();
    if (!m) return QString();
    const QModelIndex idx = g_view->currentIndex();
    if (!idx.isValid()) return QString();
    return cellText(m, idx.row(), g_cols.id);
}

static void setLockForCurrent(bool on) {
    const QString id = currentDevId();
    if (id.isEmpty()) { logf("[lock] no device selected"); return; }
    { std::lock_guard<std::mutex> lk(g_devMx);
      DevState& st = g_dev[id];
      st.locked = on; st.retries = 0; st.bad = false;
      if (on) st.lastTrigger = QDateTime::currentMSecsSinceEpoch(); }
    logf("[lock] %s dev=%s", on ? "LOCK" : "UNLOCK", id.toUtf8().constData());
    saveState();
    if (g_view && g_proxy) {
        const int col = g_proxy->extraCol();
        g_proxy->dataChanged(g_proxy->index(0, col),
                             g_proxy->index(g_proxy->rowCount() - 1, col),
                             {Qt::DisplayRole});
    }
    if (g_view) g_view->viewport()->update();
}

static QAction* hookExec(QMenu* self, const QPoint& pos, QAction* at) {
    if (!self) return g_origExec(self, pos, at);
    bool isDeviceMenu = false;
    const QList<QAction*> acts = self->actions();
    for (QAction* a : acts) {
        const QString t = a->text();
        if (t.contains(QStringLiteral("删除设备")) || t.contains(QStringLiteral("复制设备号")) ||
            t.contains(QStringLiteral("激活授权"))) { isDeviceMenu = true; break; }
    }
    if (!isDeviceMenu) return g_origExec(self, pos, at);

    QAction* aDump   = self->addAction(QStringLiteral("[EXT] 自检：导出设备列表"));
    QAction* aLock   = self->addAction(QStringLiteral("[EXT] 🔒 开启保活锁定"));
    QAction* aUnlock = self->addAction(QStringLiteral("[EXT] 🔓 取消保活锁定"));
    const QString id = currentDevId();
    bool locked = false;
    { std::lock_guard<std::mutex> lk(g_devMx); locked = g_dev.value(id).locked; }
    aLock->setEnabled(!locked);
    aUnlock->setEnabled(locked);
    logf("[menu] device menu on dev=%s locked=%d", id.toUtf8().constData(), (int)locked);

    QAction* hit = g_origExec(self, pos, at);
    if (hit == aDump)   dumpList("menu");
    else if (hit == aLock)   setLockForCurrent(true);
    else if (hit == aUnlock) setLockForCurrent(false);
    return hit;
}

static void installMenuHook() {
    HMODULE exe = GetModuleHandleW(nullptr);
    const char* kExec = "?exec@QMenu@@QEAAPEAVQAction@@AEBVQPoint@@PEAV2@@Z";
    void* orig = patchIat(exe, "Qt6Widgets.dll", kExec, (void*)&hookExec);
    if (orig) { g_origExec = (ExecFn)orig; logf("[hook] QMenu::exec patched orig=%p", orig); }
    else      { logf("[hook] !! QMenu::exec IAT not found — 右键菜单功能不可用"); }
}

// ---------------------------------------------------------------- 触发运行

static void triggerRun(int row) {
    if (!g_view) return;
    QAbstractItemModel* m = g_view->model();
    const QString id = cellText(m, row, g_cols.id);
    if (g_cfg.dryRun) { logf("[watch] DRY would trigger run row=%d dev=%s", row, id.toUtf8().constData()); return; }

    if (g_cfg.useCheck) {
        const QModelIndex ci = m->index(row, 0);
        if (m->flags(ci) & Qt::ItemIsUserCheckable)
            m->setData(ci, Qt::Checked, Qt::CheckStateRole);
    }
    g_view->selectRow(row);

    if (g_cfg.route == QStringLiteral("p2")) {
        // 直调厂家的 onRunScript 槽（在 deviceView 的祖先里找）
        QObject* owner = g_win;
        if (owner) {
            bool ok = QMetaObject::invokeMethod(owner, "onRunScript", Qt::DirectConnection);
            logf("[watch] invoke onRunScript -> %d", (int)ok);
        }
        return;
    }
    QWidget* rb = findByName(QStringLiteral("runButton"));
    if (auto* b = qobject_cast<QAbstractButton*>(rb)) {
        b->click();
        logf("[watch] clicked runButton (row=%d dev=%s)", row, id.toUtf8().constData());
    } else {
        logf("[watch] !! runButton not found");
    }
}

// ---------------------------------------------------------------- 看门狗

static qint64 g_lastSweep = 0;

static void tick() {
    if (g_win && !g_view) locateWidgets();
    if (g_cfg.enableColumn && g_view && g_proxy && g_view->model() != g_proxy) {
        logf("[column] model was replaced by app -> re-installing");
        installColumn();
    }
    if (!g_cfg.enabled) return;

    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - g_lastSweep < g_cfg.sweepMs) return;
    g_lastSweep = now;

    if (!g_view) return;
    QAbstractItemModel* m = g_view->model();
    if (!m) return;
    g_cols = locateColumns(m);
    if (g_cols.run < 0 || g_cols.id < 0) return;

    std::lock_guard<std::mutex> lk(g_devMx);
    for (auto it = g_dev.begin(); it != g_dev.end(); ++it) {
        if (!it->locked) continue;
        const QString id = it.key();
        const int row = rowOfDevId(m, g_cols.id, id);
        if (row < 0) { continue; }
        const QString run  = cellText(m, row, g_cols.run);
        const QString conn = cellText(m, row, g_cols.conn);
        const QString auth = cellText(m, row, g_cols.auth);
        if (!run.contains(QStringLiteral("运行"))) {           // 空闲/其他 => 未运行
            if (g_cols.conn >= 0 && !conn.contains(QStringLiteral("已连接"))) continue;
            if (g_cols.auth >= 0 && !auth.contains(QStringLiteral("已授权"))) continue;
            if (now - it->lastTrigger < g_cfg.cooldownMs) continue;
            if (it->retries >= g_cfg.maxRetry) {
                if (!it->bad) { it->bad = true; logf("[watch] dev=%s 连续 %d 次未起，标红停手",
                                                     id.toUtf8().constData(), it->retries); }
                continue;
            }
            logf("[watch] dev=%s 状态=%s 未运行 -> 触发 (retry=%d)", id.toUtf8().constData(),
                 run.toUtf8().constData(), it->retries);
            it->lastTrigger = now; it->retries++;
            triggerRun(row);
        } else {
            if (it->retries != 0 || it->bad) {
                it->retries = 0; it->bad = false;
                logf("[watch] dev=%s 已恢复运行", id.toUtf8().constData());
            }
        }
    }
}

// ---------------------------------------------------------------- 主线程装配

static void extSetup() {
    ++g_setupDone;
    locateWidgets();
    locateColumns(g_view ? g_view->model() : nullptr);
    g_cols = locateColumns(g_view ? g_view->model() : nullptr);
    installMenuHook();
    dumpList("init");
    if (g_cfg.enableColumn) installColumn();

    QTimer* t = new QTimer(QCoreApplication::instance());
    QObject::connect(t, &QTimer::timeout, QCoreApplication::instance(), [] { tick(); });
    t->start(2000);
    logf("[init] qt ready, timer started (tick 2000ms, sweep %dms)", g_cfg.sweepMs);
}

// ---------------------------------------------------------------- 入口

static DWORD WINAPI initThread(LPVOID) {
    for (int i = 0; i < 1800; ++i) {                 // 最长等 180s
        QCoreApplication* app = QCoreApplication::instance();
        if (app) {
            QMetaObject::invokeMethod(app, [] { extSetup(); }, Qt::QueuedConnection);
            for (int k = 0; k < 60 && !g_setupDone; ++k) Sleep(100);
            if (g_setupDone) return 0;
        }
        Sleep(100);
    }
    logf("[init] !! timeout waiting for Qt");
    return 1;
}

BOOL APIENTRY DllMain(HMODULE hSelf, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = hSelf;
        DisableThreadLibraryCalls(hSelf);
        wchar_t buf[MAX_PATH]{};
        GetModuleFileNameW(hSelf, buf, MAX_PATH);
        g_dir = buf;
        const size_t p = g_dir.find_last_of(L"\\/");
        if (p != std::wstring::npos) g_dir = g_dir.substr(0, p);

        g_log = _wfopen(fileInDir(L"bbcenter_ext.log").c_str(), L"a");
        logf("==== bbcenter_ext %s attach pid=%lu ====", BBEXT_VER, GetCurrentProcessId());
        loadConfig();
        loadState();
        CreateThread(nullptr, 0, initThread, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        logf("==== detach ====");
        if (g_log) { fclose(g_log); g_log = nullptr; }
    }
    return TRUE;
}
