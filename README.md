# BBCenter 保活增强 · PoC 工程（bbcenter_ext + BBCenterLoader）

> 目标程序：`BetaScreen/BBCenter.exe`（x64 / Qt 6.6.0 Widgets / 官方中控 v1.1.0）
> 本工程 **不修改** 目标程序一个字节：启动器挂起启动 + LoadLibrary 注入，卸载 = 删文件。

## 产出

| 文件 | 作用 |
|---|---|
| `BBCenterLoader.exe` | 启动/注入器（控制台版，便于看实时日志） |
| `bbcenter_ext.dll` | 增强 DLL（右键菜单 / 锁定列 / 保活看门狗） |
| `bbcenter_ext.ini` | 配置（exe 同目录，首次运行自动生成） |
| `bbcenter_ext.json` | 锁定清单 + 触发历史（原子写） |
| `bbcenter_ext.log` | 运行日志 |

## v0.2 修复（实测日志驱动）

| # | v0.1 现象 | 根因 | v0.2 修法 |
|---|---|---|---|
| 1 | 每右键一次就多出 3 个 `[EXT]` 项 | 厂商 `deviceMenu` 是**常驻成员 QMenu**，v0.1 每次 exec 都 `addAction` | 加项前先删掉标记为 `bbext` 的旧项（`stripExtActions`），菜单项恒为 3 项 |
| 2 | 列表看不到「保活锁定」列 | ① `enable_column=0`（默认关）② 真列走 `setModel(代理模型)`，会重建 `selectionModel` 打断厂商自己的连接 | ① 默认改 `1`；② 改为**直接给厂商那个 `QStandardItemModel` 追加一列**（不换 model 对象、不动 selectionModel、列加在**末尾**，不打乱任何既有列索引）；挂 model 信号自动补写单元格；该列单独挂 `QStyledItemDelegate` 保证能画出来 |
| 3 | `name=-1` | 标题实际是「设备别名」 | 匹配词加「设备别名」 |
| 4 | 「未运行」会被误判成运行 | `run.contains("运行")` | `looksRunning()`：先排除 未/停止/关闭/失败/断开/无，再判 运行/播放/投屏/启动 |
| 5 | 潜在自锁死 | 看门狗持 `g_devMx` 时 `triggerRun()` → 厂商刷 model → `dataChanged` 回调要锁同一把锁 | 改成「锁内只收集任务，锁外触发」 |

## PoC 分三步验证（默认保守，逐步放开）

默认配置：`enabled=1 / dry_run=1 / enable_column=1 / column_mode=insert` —— 列出列但**不点运行**。

1. **第 1 步 · 注入自检**（默认即可）
   `BBCenterLoader.exe` 启动中控 → 看 `bbcenter_ext.log`：
   - `[init] qt ready` / `[init] found deviceView=0x...` / `[init] mainWindow ok`
   - `[hook] QMenu::exec patched orig=0x...`
   - `[column] inserted extra col idx=13 (cols 13 -> 14)` / `[column] insert-mode ok`
   - `[dump] header = 序号|版本|设备号|...` 与 `[dump] row0 = ...`
   - 中控里**右键任意设备行** → 菜单应出现 3 项 `[EXT] ...`，且**反复右键数量不变**；
     点「自检：导出设备列表」会再打一份列表到日志

2. **第 2 步 · 真列**
   - 列表最右应出现「保活锁定」列，值 `— 未锁定`
   - `column_mode=insert`（默认）不改 model 对象，风险已压到最低；
     若仍异常 → 改 `enable_column=0` 回到只用右键菜单

3. **第 3 步 · 真实触发**（把 `dry_run` 改成 `0`）
   - 右键设备 → `[EXT] 开启保活锁定`
   - 把该设备脚本停掉 → 应在 ≤10s 巡检 + 冷却内被自动拉起来
   - 日志出现 `[watch] ... 未运行 -> 触发 (retry=1)`

## 构建

GitHub Actions（windows-latest，自带 MSVC）+ aqtinstall 装 **Qt 6.6.3**（`win64_msvc2019_64`，
与目标 6.6.0 同 minor、二进制兼容）。
仓库需 public 才能免费跑 CI，产物下载完即可设回 private。

## 配置项（`bbcenter_ext.ini`）

```ini
[ext]
enabled=1          ; 保活总开关
dry_run=1          ; 1=只记日志不点运行（PoC 默认）
enable_column=1    ; 1=加「保活锁定」真列
column_mode=insert ; insert=给厂商 model 直接追加一列（默认，零身份扰动）
                   ; proxy =换 view 的 model 为 QIdentityProxyModel（备选）
sweep_ms=10000     ; 巡检间隔
cooldown_ms=90000  ; 单设备最短重试间隔
max_retry=5        ; 连续失败上限（达到即标红停手 ❗）
use_check=1        ; 触发前是否同时勾选该行
require_conn=1     ; 触发前置条件：该行「连接」须为已连接
require_auth=1     ; 触发前置条件：该行「授权」须为已授权
trigger_route=p1   ; p1=selectRow+runButton.click  p2=invokeMethod(onRunScript)
```

## 改了什么 / 怎么退回去

- **不修改** `BBCenter.exe`、不改 `bbcenter.db`、不动注册表。
- 新增仅 5 个文件（本工程产物 + ini/json/log），全在 `BBCenter.exe` 同目录。
- **回滚**：直接双击原 `BBCenter.exe` 启动（不经过加载器）= 增强完全不生效；
  彻底清理 = 删掉那 5 个文件。
