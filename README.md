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

## PoC 分三步验证（默认最保守，逐步放开）

默认配置：`enabled=1 / dry_run=1 / enable_column=0` —— **只看不动手**。

1. **第 1 步 · 注入自检**（默认即可）
   `BBCenterLoader.exe` 启动中控 → 看 `bbcenter_ext.log`：
   - `[init] qt ready` / `[init] found deviceView=0x...` / `[init] mainWindow ok`
   - `[hook] QMenu::exec patched orig=0x...`
   - `[dump] header = 序号|版本|设备号|...` 与 `[dump] row0 = ...`
   - 中控里**右键任意设备行** → 菜单应出现三项 `[EXT] ...`，点「自检：导出设备列表」会再打一份列表到日志
   - `enable_column=0` 时**不动列表结构**

2. **第 2 步 · 真列**（把 `enable_column` 改成 `1`，重启中控）
   - 列表最右应出现「保活锁定」列（默认显示「— 未锁定」）
   - ★ 这一步是本 PoC 的**最高风险项**：换 view 的 model 为代理模型，若中控内部对
     `deviceView->model()` 做了类型假设，可能出现异常 → **立刻把 `enable_column` 改回 `0`**，
     我们改为「旁列贴片」方案（不改 model，自己贴一列，零风险）

3. **第 3 步 · 真实触发**（把 `dry_run` 改成 `0`）
   - 右键设备 → `[EXT] 开启保活锁定`
   - 把该设备脚本停掉 → 应在 ≤10s 巡检 + 冷却内被自动拉起来
   - 日志出现 `[watch] trigger run row=N dev=...`

## 构建

GitHub Actions（windows-latest，自带 MSVC）+ aqtinstall 装 Qt 6.6.0（`win64_msvc2019_64`）。
仓库需 public 才能免费跑 CI，产物下载完即可设回 private。

## 配置项（`bbcenter_ext.ini`）

```ini
[ext]
enabled=1          ; 保活总开关
dry_run=1          ; 1=只记日志不点运行（PoC 默认）
enable_column=0    ; 1=换代理模型加「保活锁定」真列
sweep_ms=10000     ; 巡检间隔
cooldown_ms=90000  ; 单设备最短重试间隔
max_retry=5        ; 连续失败上限（达到即标红停手）
use_check=1        ; 触发前是否同时勾选该行
trigger_route=p1   ; p1=selectRow+runButton.click  p2=invokeMethod(onRunScript)
```

## 改了什么 / 怎么退回去

- **不修改** `BBCenter.exe`、不改 `bbcenter.db`、不动注册表。
- 新增仅 5 个文件（本工程产物 + ini/json/log），全在 `BBCenter.exe` 同目录。
- **回滚**：直接双击原 `BBCenter.exe` 启动（不经过加载器）= 增强完全不生效；
  彻底清理 = 删掉那 5 个文件。
