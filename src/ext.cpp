// bbcenter_ext.dll — BBCenter（贝塔投屏中控）保活增强 v0.2
// 不修改目标程序字节：仅进程内 hook + 调用 Qt 公开 API。
// 挂载点：① 右键设备菜单追加「保活锁定」 ② 列表真列「保活锁定」 ③ 主线程看门狗
//
// v0.2 修复
//  1. 右键菜单项累加：厂商 deviceMenu 是常驻成员 QMenu，v0.1 每次 exec 都 addAction
//     -> 改为「先清掉自己上一次加的（objectName=bbext），再追加」，菜单项恒为 3 个。
//  2. 列表没出现「保活锁定」列：
//     a) 默认 enable_column 改为 1（v0.1 是 0，等于没开）；
//     b) ★真列实现从「换 view 的 model（QIdentityProxyModel）」改为
//        「直接给厂商那个 QStandardItemModel 追加一列」——不换 model 对象、
//        不重建 selectionModel、app 持有的 model 指针身份不变，零身份扰动；
//        并挂 model 信号（modelReset/rowsInserted/rowsRemoved/dataChanged/layoutChanged）
//        自动补写单元格，厂商整表刷新也不会丢列。
//     c) 该列单独挂 QStyledItemDelegate，保证厂商自定义 delegate 不画我们的列时也能显示。
//  3. 列标题匹配修正：厂商实际是「设备别名」不是「设备名称」（v0.1 name=-1）。
//  4. 运行态判定修正：空闲值为「--」；v0.1 用 contains("运行") 会把「未运行」误判为运行。
//  5. 死锁修复：看门狗持有 g_devMx 时调用 triggerRun -> app 刷新 model -> dataChanged
//     回调要锁 g_devMx -> 自锁死。改为「锁内只收集任务，锁外触发」。
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
#include <QStandardItemModel>
#include <QStandardItem>
#include <QStyledItemDelegate>
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
    bool enableColumn = true;              // v0.2: 默认开
    bool useCheck = false;                 // v0.3: 默认不碰勾选列（避免误影响其他设备）
    bool requireConn = true;
    bool requireAuth = true;
    bool confirmByButtons = true;          // v0.3: 动手前用 runButton/stopButton 状态二次确认
    bool unknownIsRunning = true;          // v0.3: 运行态认不出时按「在跑」处理（不乱点，保守）
    int  sweepMs = 10000;
    int  cooldownMs = 90000;
    int  maxRetry = 5;
    QString route = QStringLiteral("p1");
    QString columnMode = QStringLiteral("insert");   // insert | proxy
    QString runningWords;                  // 「在跑」关键字（逗号分隔）
    QString idleMarkers;                   // 「不在跑」关键字（逗号分隔）
} g_cfg;

static QList<QString> splitWords(const QString& s) {
    QList<QString> out;
    for (const QString& p : s.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString t = p.trimmed();
        if (!t.isEmpty()) out << t;
    }
    return out;
}
static QList<QString> g_runningWords, g_idleMarkers;

static void loadConfig() {
    const QString ini = qFileInDir(L"bbcenter_ext.ini");
    if (!QFile::exists(ini)) {
        QFile f(ini);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            f.write("[ext]\n"
                    "enabled=1\n"
                    "dry_run=1\n"
                    "enable_column=1\n"
                    "column_mode=insert\n"
                    "sweep_ms=10000\n"
                    "cooldown_ms=90000\n"
                    "max_retry=5\n"
                    "use_check=0\n"
                    "require_conn=1\n"
                    "require_auth=1\n"
                    "confirm_by_buttons=1\n"
                    "unknown_is_running=1\n"
                    "running_words=运行,播放,投屏,启动,工作中,已开,开始\n"
                    "idle_markers=未,停止,关闭,失败,断开,无,空闲,空,不在线\n"
                    "trigger_route=p1\n");
            f.close();
        }
    }
    QSettings s(ini, QSettings::IniFormat);
    g_cfg.enabled          = s.value("ext/enabled", 1).toInt() != 0;
    g_cfg.dryRun           = s.value("ext/dry_run", 1).toInt() != 0;
    g_cfg.enableColumn     = s.value("ext/enable_column", 1).toInt() != 0;
    g_cfg.useCheck         = s.value("ext/use_check", 0).toInt() != 0;
    g_cfg.requireConn      = s.value("ext/require_conn", 1).toInt() != 0;
    g_cfg.requireAuth      = s.value("ext/require_auth", 1).toInt() != 0;
    g_cfg.confirmByButtons = s.value("ext/confirm_by_buttons", 1).toInt() != 0;
    g_cfg.unknownIsRunning = s.value("ext/unknown_is_running", 1).toInt() != 0;
    g_cfg.sweepMs          = s.value("ext/sweep_ms", 10000).toInt();
    g_cfg.cooldownMs       = s.value("ext/cooldown_ms", 90000).toInt();
    g_cfg.maxRetry         = s.value("ext/max_retry", 5).toInt();
    g_cfg.route            = s.value("ext/trigger_route", "p1").toString();
    g_cfg.columnMode       = s.value("ext/column_mode", "insert").toString().trimmed().toLower();
    if (g_cfg.columnMode != QStringLiteral("proxy")) g_cfg.columnMode = QStringLiteral("insert");
    g_cfg.runningWords     = s.value("ext/running_words", "运行,播放,投屏,启动,工作中,已开,开始").toString();
    g_cfg.idleMarkers      = s.value("ext/idle_markers", "未,停止,关闭,失败,断开,无,空闲,空,不在线").toString();
    g_runningWords = splitWords(g_cfg.runningWords);
    g_idleMarkers  = splitWords(g_cfg.idleMarkers);
    logf("[cfg] enabled=%d dry_run=%d enable_column=%d column_mode=%s sweep=%dms cooldown=%dms max_retry=%d route=%s",
         (int)g_cfg.enabled, (int)g_cfg.dryRun, (int)g_cfg.enableColumn,
         g_cfg.columnMode.toUtf8().constData(),
         g_cfg.sweepMs, g_cfg.cooldownMs, g_cfg.maxRetry, g_cfg.route.toUtf8().constData());
    logf("[cfg] use_check=%d conn=%d auth=%d confirm_by_buttons=%d unknown_is_running=%d",
         (int)g_cfg.useCheck, (int)g_cfg.requireConn, (int)g_cfg.requireAuth,
         (int)g_cfg.confirmByButtons, (int)g_cfg.unknownIsRunning);
    QString rw; for (const QString& w : g_runningWords) rw += w + QLatin1Char('|');
    QString iw; for (const QString& w : g_idleMarkers) iw += w + QLatin1Char('|');
    logf("[cfg] running_words=%s idle_markers=%s", rw.toUtf8().constData(), iw.toUtf8().constData());
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
    QJsonObject root, devs;
    {
        std::lock_guard<std::mutex> lk(g_devMx);
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
    }
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
    {
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
        else if (h == QStringLiteral("设备别名") || h == QStringLiteral("设备名称")) c.name = i;
        else if (h == QStringLiteral("备注")) c.remark = i;
    }
    return c;
}

static QString cellText(QAbstractItemModel* m, int row, int col) {
    if (!m || col < 0 || row < 0 || row >= m->rowCount() || col >= m->columnCount()) return QString();
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

// ---------------------------------------------------------------- 运行态判定
// 「脚本在运行吗」——先看「不在跑」标记，再看「在跑」关键字，最后兜底。
// 兜底默认按「在跑」处理：认不出来时宁可不点，绝不乱点（unknown_is_running=1）。
static bool looksRunning(const QString& raw) {
    const QString t = raw.trimmed();
    if (t.isEmpty()) return false;
    static const QStringList kDash = {
        QStringLiteral("-"), QStringLiteral("--"), QStringLiteral("---"),
        QStringLiteral("—"), QStringLiteral("——"), QStringLiteral("－")
    };
    if (kDash.contains(t)) return false;
    for (const QString& w : g_idleMarkers)  if (t.contains(w)) return false;
    for (const QString& w : g_runningWords) if (t.contains(w)) return true;
    return g_cfg.unknownIsRunning;
}

// ---------------------------------------------------------------- 锁定态文本

static QString lockTextOf(const QString& id) {
    bool locked = false, bad = false;
    {
        std::lock_guard<std::mutex> lk(g_devMx);
        auto it = g_dev.constFind(id);
        if (it != g_dev.constEnd()) { locked = it->locked; bad = it->bad; }
    }
    if (bad)    return QStringLiteral("❗ 异常停手");
    if (locked) return QStringLiteral("🔒 已锁定");
    return QStringLiteral("— 未锁定");
}

// ---------------------------------------------------------------- 真列 · 模式 insert（默认）
// 直接给厂商的 QStandardItemModel 追加一列：不换 model 对象、不动 selectionModel。

static Cols g_cols;
static QStandardItemModel* g_std = nullptr;
static QAbstractItemModel* g_hookedModel = nullptr;
static int  g_extCol = -1;
static bool g_inRefresh = false;
static QStyledItemDelegate* g_extDelegate = nullptr;

static void applyColumn();          // fwd
static void dumpButtons(const char* why);   // fwd
static void dumpStatus(const char* why);    // fwd

static void refreshExtColumn() {
    if (g_inRefresh) return;
    if (!g_view || !g_std || g_extCol < 0) return;
    if (g_extCol >= g_std->columnCount()) return;
    if (g_cols.id < 0) return;
    g_inRefresh = true;
    const int n = g_std->rowCount();
    for (int r = 0; r < n; ++r) {
        const QString id = cellText(g_std, r, g_cols.id);
        if (id.isEmpty()) continue;
        const QString txt = lockTextOf(id);
        QStandardItem* it = g_std->item(r, g_extCol);
        if (!it) { it = new QStandardItem(); g_std->setItem(r, g_extCol, it); }
        if (it->text() != txt) it->setText(txt);
        it->setTextAlignment(Qt::AlignCenter);
        it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    }
    g_inRefresh = false;
}

static void hookModelSignals(QAbstractItemModel* m) {
    if (!m || g_hookedModel == m) return;
    g_hookedModel = m;
    // modelReset 里不动模型（厂商可能 clear 后立刻重建），推到事件循环再补列，避免在信号发射中改模型
    QObject::connect(m, &QAbstractItemModel::modelReset, m, [m] { QTimer::singleShot(0, m, [] { applyColumn(); }); });
    QObject::connect(m, &QAbstractItemModel::columnsInserted, m,
                     [](const QModelIndex&, int, int) { refreshExtColumn(); });
    QObject::connect(m, &QAbstractItemModel::rowsInserted, m,
                     [](const QModelIndex&, int, int) { refreshExtColumn(); });
    QObject::connect(m, &QAbstractItemModel::rowsRemoved, m,
                     [](const QModelIndex&, int, int) { refreshExtColumn(); });
    QObject::connect(m, &QAbstractItemModel::layoutChanged, m, [] { refreshExtColumn(); });
    QObject::connect(m, &QAbstractItemModel::dataChanged, m,
                     [](const QModelIndex&, const QModelIndex&, const QList<int>&) { refreshExtColumn(); });
    logf("[column] model signals hooked (%p)", (void*)m);
}

static void applyColumnInsert() {
    if (!g_view) return;
    QAbstractItemModel* m = g_view->model();
    auto* stdm = qobject_cast<QStandardItemModel*>(m);
    if (!stdm) {
        logf("[column] !! model 不是 QStandardItemModel（%s）— 请改 column_mode=proxy",
             m ? m->metaObject()->className() : "null");
        return;
    }
    if (g_std != stdm) { g_std = stdm; g_extCol = -1; }

    if (g_extCol < 0 || stdm->columnCount() <= g_extCol) {
        const int old = stdm->columnCount();
        g_extCol = old;
        stdm->setColumnCount(old + 1);
        logf("[column] inserted extra col idx=%d (cols %d -> %d)", g_extCol, old, stdm->columnCount());
    }
    if (stdm->headerData(g_extCol, Qt::Horizontal, Qt::DisplayRole).toString()
            != QStringLiteral("保活锁定")) {
        stdm->setHeaderData(g_extCol, Qt::Horizontal, QStringLiteral("保活锁定"));
    }
    g_view->setColumnWidth(g_extCol, 96);
    if (!g_extDelegate) {
        g_extDelegate = new QStyledItemDelegate(g_view);
        g_view->setItemDelegateForColumn(g_extCol, g_extDelegate);
        logf("[column] per-column delegate installed on col %d", g_extCol);
    }
    hookModelSignals(stdm);
    refreshExtColumn();
    logf("[column] insert-mode ok, extCol=%d cols=%d rows=%d",
         g_extCol, stdm->columnCount(), stdm->rowCount());
}

// ---------------------------------------------------------------- 真列 · 模式 proxy（备选）
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
            if (role == Qt::DisplayRole) return lockTextOf(id);
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

static void applyColumnProxy() {
    if (!g_view) return;
    QAbstractItemModel* src = g_view->model();
    if (!src) return;
    if (g_proxy && src == g_proxy) return;
    if (dynamic_cast<LockProxy*>(src)) return;
    g_proxy = new LockProxy(g_view);
    g_proxy->setSourceModel(src);
    QAbstractItemModel* keep = src;
    g_view->setModel(g_proxy);
    if (g_view->model() != g_proxy) {
        logf("[column] !! proxy swap failed, restoring");
        g_view->setModel(keep);
        delete g_proxy; g_proxy = nullptr;
        return;
    }
    g_view->setColumnWidth(g_proxy->extraCol(), 96);
    logf("[column] proxy-mode ok, extCol=%d", g_proxy->extraCol());
}

static void applyColumn() {
    if (!g_cfg.enableColumn || !g_view) return;
    if (g_cfg.columnMode == QStringLiteral("proxy")) applyColumnProxy();
    else applyColumnInsert();
}

// 每拍轻量巡检：模型被换/列被厂商刷掉时自动补回
static void ensureColumn() {
    if (!g_cfg.enableColumn || !g_view) return;
    if (g_cfg.columnMode == QStringLiteral("proxy")) {
        if (!g_proxy || g_view->model() != g_proxy) {
            if (!dynamic_cast<LockProxy*>(g_view->model())) applyColumnProxy();
        }
        return;
    }
    auto* stdm = qobject_cast<QStandardItemModel*>(g_view->model());
    if (!stdm) return;
    const bool need =
        (g_std != stdm) || (g_extCol < 0) || (stdm->columnCount() <= g_extCol) ||
        (stdm->headerData(g_extCol, Qt::Horizontal, Qt::DisplayRole).toString() != QStringLiteral("保活锁定"));
    if (need) {
        logf("[column] ensure -> re-apply (model=%p extCol=%d cols=%d)", (void*)stdm, g_extCol, stdm->columnCount());
        applyColumnInsert();
    } else {
        refreshExtColumn();
    }
}

static void refreshColumnNow() {
    if (!g_cfg.enableColumn) return;
    if (g_cfg.columnMode == QStringLiteral("proxy")) {
        if (g_proxy) {
            const int col = g_proxy->extraCol();
            if (g_proxy->rowCount() > 0)
                g_proxy->dataChanged(g_proxy->index(0, col), g_proxy->index(g_proxy->rowCount() - 1, col),
                                     {Qt::DisplayRole});
        }
    } else {
        refreshExtColumn();
    }
    if (g_view) g_view->viewport()->update();
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

// 我们自己加的菜单项统一打这个标记，便于下次先清后加（修 v0.1 累加）
static const char* kExtMark = "bbext";

static void stripExtActions(QMenu* m) {
    const QList<QAction*> acts = m->actions();
    for (QAction* a : acts) {
        if (a->objectName() == QLatin1String(kExtMark)) {
            m->removeAction(a);
            a->deleteLater();
        }
    }
}

static int currentRow() {
    if (!g_view) return -1;
    const QModelIndex idx = g_view->currentIndex();
    if (idx.isValid()) return idx.row();
    if (QItemSelectionModel* sm = g_view->selectionModel()) {
        const QModelIndex c = sm->currentIndex();
        if (c.isValid()) return c.row();
    }
    return -1;
}

static QString rowDevId(int row) {
    if (row < 0 || !g_view) return QString();
    return cellText(g_view->model(), row, g_cols.id);
}

static void setLockForRow(int row, bool on) {
    const QString id = rowDevId(row);
    if (id.isEmpty()) { logf("[lock] no device at row=%d", row); return; }
    {
        std::lock_guard<std::mutex> lk(g_devMx);
        DevState& st = g_dev[id];
        st.locked = on; st.retries = 0; st.bad = false;
        if (on) st.lastTrigger = QDateTime::currentMSecsSinceEpoch();
    }
    logf("[lock] %s dev=%s row=%d", on ? "LOCK" : "UNLOCK", id.toUtf8().constData(), row);
    saveState();
    refreshColumnNow();
}

static QAction* hookExec(QMenu* self, const QPoint& pos, QAction* at) {
    if (!self || !g_cfg.enabled) return g_origExec(self, pos, at);

    bool isDeviceMenu = false;
    for (QAction* a : self->actions()) {
        const QString t = a->text();
        if (t.contains(QStringLiteral("删除设备")) || t.contains(QStringLiteral("复制设备号")) ||
            t.contains(QStringLiteral("激活授权"))) { isDeviceMenu = true; break; }
    }
    if (!isDeviceMenu) return g_origExec(self, pos, at);

    stripExtActions(self);                       // ★ 关键：先清掉上一次加的，防止累加

    QAction* sep     = self->addSeparator();
    QAction* aDump   = self->addAction(QStringLiteral("[EXT] 自检：导出设备列表"));
    QAction* aLock   = self->addAction(QStringLiteral("[EXT] 🔒 开启保活锁定"));
    QAction* aUnlock = self->addAction(QStringLiteral("[EXT] 🔓 取消保活锁定"));
    for (QAction* a : {sep, aDump, aLock, aUnlock}) a->setObjectName(QLatin1String(kExtMark));

    const int row = currentRow();
    const QString id = rowDevId(row);
    bool locked = false;
    { std::lock_guard<std::mutex> lk(g_devMx); locked = g_dev.value(id).locked; }
    aLock->setEnabled(!locked);
    aUnlock->setEnabled(locked);
    logf("[menu] device menu row=%d dev=%s locked=%d (menu items now=%d)",
         row, id.toUtf8().constData(), (int)locked, (int)self->actions().size());

    QAction* hit = g_origExec(self, pos, at);
    if (hit == aDump) { dumpList("menu"); dumpButtons("menu"); dumpStatus("menu"); }
    else if (hit == aLock)   setLockForRow(row, true);
    else if (hit == aUnlock) setLockForRow(row, false);
    return hit;
}

static void installMenuHook() {
    HMODULE exe = GetModuleHandleW(nullptr);
    const char* kExec = "?exec@QMenu@@QEAAPEAVQAction@@AEBVQPoint@@PEAV2@@Z";
    void* orig = patchIat(exe, "Qt6Widgets.dll", kExec, (void*)&hookExec);
    if (orig) { g_origExec = (ExecFn)orig; logf("[hook] QMenu::exec patched orig=%p", orig); }
    else      { logf("[hook] !! QMenu::exec IAT not found — 右键菜单功能不可用"); }
}

// ---------------------------------------------------------------- 按钮（中控权威状态来源）

static QAbstractButton* findButton(const QString& name) {
    if (g_win) if (auto* b = g_win->findChild<QAbstractButton*>(name)) return b;
    if (QWidget* w = findByName(name)) return qobject_cast<QAbstractButton*>(w);
    return nullptr;
}

// 一次性把主窗口所有按钮名/文本/可用状态打进日志 —— 确认 runButton/stopButton 就位
static void dumpButtons(const char* why) {
    if (!g_win) { logf("[btn:%s] no main window", why); return; }
    const QList<QAbstractButton*> bs = g_win->findChildren<QAbstractButton*>();
    logf("[btn:%s] total=%d", why, (int)bs.size());
    for (QAbstractButton* b : bs) {
        logf("[btn:%s] name='%s' text='%s' enabled=%d visible=%d class=%s", why,
             b->objectName().toUtf8().constData(), b->text().toUtf8().constData(),
             (int)b->isEnabled(), (int)b->isVisible(), b->metaObject()->className());
    }
}

// ---------------------------------------------------------------- 触发运行
// 目标 = 与人工操作等价：选中该设备行 -> 点中控工具栏「运行」。
// v0.3：动手前用 stopButton/runButton 的可用状态做二次确认（中控自己的权威判定）：
//   · 选中后 stopButton 可用  -> 设备正在跑，不需要点
//   · 选中后 runButton 不可用 -> 设备当前不可运行（离线/未授权），本轮跳过
// 返回 true = 已达成「在跑」或成功触发（重置重试计数）；false = 软失败。
static QAbstractButton* g_runBtn = nullptr;
static QAbstractButton* g_stopBtn = nullptr;

static bool triggerRun(int row, const char* why) {
    if (!g_view) return false;
    QAbstractItemModel* m = g_view->model();
    const QString id = cellText(m, row, g_cols.id);
    if (g_cfg.dryRun) {
        logf("[watch] DRY would trigger run row=%d dev=%s (%s)", row, id.toUtf8().constData(), why);
        return true;
    }

    if (!g_runBtn)  g_runBtn  = findButton(QStringLiteral("runButton"));
    if (!g_stopBtn) g_stopBtn = findButton(QStringLiteral("stopButton"));

    // 透明度保护：若中控的「运行」是「批量作用于勾选行」的语义，别的设备会被一起启动。
    int checkedOthers = 0;
    for (int r = 0, n = m->rowCount(); r < n; ++r) {
        if (r == row) continue;
        const QModelIndex ci = m->index(r, 0);
        if ((m->flags(ci) & Qt::ItemIsUserCheckable) &&
            m->data(ci, Qt::CheckStateRole).toInt() == Qt::Checked)
            ++checkedOthers;
    }
    if (checkedOthers > 0)
        logf("[watch] WARN 另有 %d 行处于勾选状态；若中控「运行」作用于勾选行，它们会被一并启动",
             checkedOthers);

    g_view->selectRow(row);                       // 等价人工点中这一行
    if (g_cfg.useCheck) {
        const QModelIndex ci = m->index(row, 0);
        if (m->flags(ci) & Qt::ItemIsUserCheckable)
            m->setData(ci, Qt::Checked, Qt::CheckStateRole);
    }

    if (g_cfg.confirmByButtons) {
        if (g_stopBtn && g_stopBtn->isEnabled()) {
            logf("[watch] dev=%s 选中后 stopButton=可用 -> 判定在跑，不点", id.toUtf8().constData());
            return true;
        }
        if (g_runBtn && !g_runBtn->isEnabled()) {
            logf("[watch] dev=%s 选中后 runButton=不可用 -> 设备不可运行，跳过", id.toUtf8().constData());
            return false;
        }
    }

    if (g_cfg.route == QStringLiteral("p2")) {
        QObject* owner = g_win;
        if (owner) {
            bool ok = QMetaObject::invokeMethod(owner, "on_runButton_clicked", Qt::DirectConnection);
            if (!ok) ok = QMetaObject::invokeMethod(owner, "onRunScript", Qt::DirectConnection);
            logf("[watch] invoke on_runButton_clicked/onRunScript -> %d (row=%d dev=%s)", (int)ok, row,
                 id.toUtf8().constData());
            return ok;
        }
        return false;
    }

    if (!g_runBtn) { logf("[watch] !! runButton not found（见 [btn:*] 日志），why=%s", why); return false; }
    g_runBtn->click();
    logf("[watch] clicked runButton (row=%d dev=%s, stopEnabled=%d, why=%s)",
         row, id.toUtf8().constData(), (int)(g_stopBtn ? g_stopBtn->isEnabled() : -1), why);
    return true;
}

// ---------------------------------------------------------------- 看门狗

static qint64 g_lastSweep = 0;
static QHash<QString, QString> g_lastRun;        // devId -> 上一次看到的「运行」列原文（主线程私有）

// 一次性快照：每行「运行」列原文 + 我们的判定 —— 用于核对判定是否与中控一致
static void dumpStatus(const char* why) {
    if (!g_view) return;
    QAbstractItemModel* m = g_view->model();
    if (!m) return;
    const Cols c = locateColumns(m);
    logf("[stat:%s] rows=%d idCol=%d runCol=%d connCol=%d authCol=%d", why, m->rowCount(),
         c.id, c.run, c.conn, c.auth);
    for (int r = 0, n = m->rowCount(); r < n; ++r) {
        const QString run = cellText(m, r, c.run);
        logf("[stat:%s] row%d dev=%s 运行原文='%s' 判定=%s 连接='%s' 授权='%s'", why, r,
             cellText(m, r, c.id).toUtf8().constData(), run.toUtf8().constData(),
             looksRunning(run) ? "在跑" : "不在跑",
             cellText(m, r, c.conn).toUtf8().constData(),
             cellText(m, r, c.auth).toUtf8().constData());
    }
}

// 运行态变化留痕：抓真实文本，用于校准 running_words/idle_markers
static void logStatusChanges(QAbstractItemModel* m) {
    if (!m || g_cols.run < 0 || g_cols.id < 0) return;
    for (int r = 0, n = m->rowCount(); r < n; ++r) {
        const QString id = cellText(m, r, g_cols.id);
        if (id.isEmpty()) continue;
        const QString st = cellText(m, r, g_cols.run);
        auto it = g_lastRun.find(id);
        if (it == g_lastRun.end()) { g_lastRun[id] = st; continue; }
        if (it.value() == st) continue;
        const QString old = it.value();
        it.value() = st;
        logf("[status] dev=%s 运行 '%s' -> '%s'  判定=%s | 连接='%s' 授权='%s'",
             id.toUtf8().constData(), old.toUtf8().constData(), st.toUtf8().constData(),
             looksRunning(st) ? "在跑" : "不在跑",
             cellText(m, r, g_cols.conn).toUtf8().constData(),
             cellText(m, r, g_cols.auth).toUtf8().constData());
    }
}

static void sweep() {
    if (!g_view) return;
    QAbstractItemModel* m = g_view->model();
    if (!m) return;
    g_cols = locateColumns(m);
    if (g_cols.run < 0 || g_cols.id < 0) {
        logf("[watch] !! 列定位失败 (id=%d run=%d)，跳过本轮", g_cols.id, g_cols.run);
        return;
    }
    logStatusChanges(m);

    struct Job { QString id; int row; const char* why; };
    QList<Job> jobs;
    bool changed = false;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    {
        std::lock_guard<std::mutex> lk(g_devMx);
        for (auto it = g_dev.begin(); it != g_dev.end(); ++it) {
            if (!it->locked || it->bad) continue;
            const QString id = it.key();
            const int row = rowOfDevId(m, g_cols.id, id);
            if (row < 0) continue;
            const QString run = cellText(m, row, g_cols.run);

            if (looksRunning(run)) {
                if (it->retries != 0) {
                    it->retries = 0; changed = true;
                    logf("[watch] dev=%s 已恢复运行", id.toUtf8().constData());
                }
                continue;
            }
            if (g_cfg.requireConn && g_cols.conn >= 0 &&
                !cellText(m, row, g_cols.conn).contains(QStringLiteral("已连接"))) continue;
            if (g_cfg.requireAuth && g_cols.auth >= 0 &&
                !cellText(m, row, g_cols.auth).contains(QStringLiteral("已授权"))) continue;
            if (now - it->lastTrigger < g_cfg.cooldownMs) continue;

            if (it->retries >= g_cfg.maxRetry) {
                it->bad = true; changed = true;
                logf("[watch] dev=%s 连续 %d 次未起 -> 标红停手", id.toUtf8().constData(), it->retries);
                continue;
            }
            it->lastTrigger = now;
            it->retries++;
            changed = true;
            jobs.append({id, row, "运行列为空/非运行态"});
            logf("[watch] dev=%s 运行列原文='%s' -> 判定未运行，准备触发 (retry=%d)",
                 id.toUtf8().constData(), run.toUtf8().constData(), it->retries);
        }
    }

    for (const Job& j : jobs) {
        if (triggerRun(j.row, j.why)) {
            std::lock_guard<std::mutex> lk(g_devMx);
            auto it = g_dev.find(j.id);
            if (it != g_dev.end()) it->retries = 0;      // 已达成 / 已触发：不累计失败
            changed = true;
        }
    }
    if (changed) saveState();
    refreshExtColumn();
}

static void tick() {
    if (g_win && !g_view) locateWidgets();
    ensureColumn();                      // ★ 必须在取 g_devMx 之前（内部会读锁定态）
    if (!g_cfg.enabled) return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - g_lastSweep < g_cfg.sweepMs) return;
    g_lastSweep = now;
    sweep();
}

// ---------------------------------------------------------------- 主线程装配

static void extSetup() {
    ++g_setupDone;
    locateWidgets();
    g_cols = locateColumns(g_view ? g_view->model() : nullptr);
    installMenuHook();
    dumpList("init");
    dumpButtons("init");
    dumpStatus("init");
    if (g_cfg.enableColumn) applyColumn();
    else logf("[column] disabled (enable_column=0)");

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
