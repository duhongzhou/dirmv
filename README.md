# dirMover — 目录软链接迁移/恢复工具

将监视目录中的**新建、移入或原有的非白名单目录**自动迁移到目标目录，并在原位置创建
目录链接（符号链接 / 目录联接），使依赖这些目录的软件仍能正常运行。

由原批处理脚本 `迁移软件数据.bat` 移植为完整的 C++ / Win32 桌面项目。

---

## 功能特性

| 功能         | 说明                                                                                  |
| ------------ | ------------------------------------------------------------------------------------- |
| 目录迁移     | 把监视目录下每个非白名单一级子目录迁移到目标目录                                      |
| 创建链接     | 原位置创建**符号链接**（默认，需管理员）或**目录联接 Junction**                       |
| 白名单       | 内置系统目录白名单 + 用户自定义排除列表（每行一个）                                   |
| 自动迁移     | ① 文件系统实时监视（`ReadDirectoryChangesW`，新目录立即迁移）② 定时扫描（可配置间隔） |
| 进度与 ETA   | 迁移时显示进度条、已传输字节、速度与**预计完成时间**                                  |
| 恢复         | 删除链接、把数据移回原位置（依据迁移日志 LINK\|src\|dst）                             |
| 回滚         | 创建链接失败时自动把数据移回源目录                                                    |
| 多线程       | 前台 / 后台运行均可限制线程数（1–64）                                                 |
| Windows 服务 | 注册为系统服务、**开机自动启动**、常驻后台自动/定时迁移                               |
| 后台控制     | GUI 通过命名管道远程控制后台服务（迁移 / 恢复 / 状态 / 停止 / 重载）                  |
| 系统托盘     | 关闭窗口最小化到托盘，右键菜单可快速操作                                              |
| 命令行       | `dirMover_cli.exe` 支持脚本自动化与无界面环境                                         |

---

## 系统 API 使用

项目全部基于原生 Win32 API（无第三方依赖）：

- 目录创建：`CreateDirectoryW`
- 目录枚举：`FindFirstFileExW` / `FindNextFileW`
- 文件复制（带进度）：`CopyFileExW` + `CopyProgressRoutine`
- 移动 / 删除：`MoveFileExW`、`DeleteFileW`、`RemoveDirectoryW`
- 链接创建：`CreateSymbolicLinkW`（符号链接 / 目录联接）
- 目录监视：`ReadDirectoryChangesW`
- 服务：`CreateServiceW` / `StartServiceCtrlDispatcherW` / `RegisterServiceCtrlHandlerExW`
- 进程间通信：命名管道 `CreateNamedPipeW` / `CreateFileW`
- 托盘通知（"honk"）：`Shell_NotifyIconW`

---

## 构建

### 依赖

- Visual Studio 2022/2026（含 MSVC）
- CMake ≥ 3.20

### 一键构建

```bat
build.bat            :: Release
build.bat Debug      :: Debug
```

或手动：

```bat
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

> 生成器名称请按本机 VS 版本调整（如 `"Visual Studio 17 2022"`）。

输出：

- `build/Release/dirMover.exe` —— 图形界面 / 服务主程序
- `build/Release/dirMover_cli.exe` —— 命令行工具

---

## 在 VS Code 中运行与调试（一键启动）

项目已内置 `.vscode/` 配置，**打开项目后按 `F5` 即可一键构建并启动调试**。

### 调试配置（`launch.json`）

| 配置名                     | 说明                                                          |
| -------------------------- | ------------------------------------------------------------- |
| **调试 dirMover (GUI)**    | 默认。构建后启动图形界面，附加调试器（外部窗口）              |
| 调试 dirMover (Debug)      | 构建 Debug 版本（含完整符号）后调试 GUI                       |
| 调试 dirMover_cli (命令行) | 构建后以 `scan` 运行命令行工具，集成终端输出                  |
| 运行迁移 (CLI migrate)     | 构建后执行一次 `migrate`，便于观察进度与占用提示              |
| 调试 服务模式 (--service)  | 以 `--service` 启动，用于调试服务逻辑（需管理员运行 VS Code） |

### 任务（`tasks.json`，`Ctrl+Shift+B` 构建）

- `CMake: 构建 (Release)` —— 默认构建任务
- `CMake: 构建 (Debug)`
- `CMake: 配置` / `CMake: 重新配置 (清缓存)`
- `运行 GUI (Release)` / `运行 CLI: scan` / `运行 CLI: migrate`

### 一键启动步骤

1. 用 VS Code 打开项目根目录 `dirmv`
2. 按 **`F5`** → 自动执行 `CMake: 配置` → `CMake: 构建 (Release)` → 启动 `dirMover.exe` 并附加调试器
3. 需要以管理员身份测试（创建符号链接 / 安装服务）时，请以管理员身份启动 VS Code 后再 `F5`

> 若本机 Visual Studio 版本不是 2026，请把 `tasks.json`、`settings.json` 与 `build.bat` 中的
> `"Visual Studio 18 2026"` 改成你的生成器（如 `"Visual Studio 17 2022"`）。

---

## 功能测试

本应用是**原生 Win32 桌面程序**，可以用脚本控制，共提供三套测试：

```powershell
# 一键运行全部测试套件（推荐）
powershell -ExecutionPolicy Bypass -File test\run_all_tests.ps1
```

| 套件                   | 文件                 | 说明                                                                           |
| ---------------------- | -------------------- | ------------------------------------------------------------------------------ |
| **CLI 全功能测试**     | `test\clitest.ps1`   | 用「测试规则文件」通过 `dirMover_cli.exe` 驱动，逐项验证 **19** 个功能点       |
| **GUI 自动化冒烟测试** | `test\guitest.ps1`   | 启动真实 GUI 窗口，用 `EnumWindows/SendMessage` 驱动，验证界面可用（**9** 项） |
| **综合功能测试**       | `test\run_tests.ps1` | 原有全流程综合测试（**57** 项）                                                |

### 测试规则文件

`test\rules\dirmv.test.ini.template` 是规则的模板；实际测试时由 `TestCommon.psm1` 的
`New-TestRuleFile` **在运行时生成**指向 `test\sandbox` 的规则文件（因为沙箱绝对路径因机器而异），
供 CLI / GUI 测试反复导入使用。

### 安全护栏（不碰真实目录）

- `test\TestCommon.psm1` 的 **`Assert-InSandbox`** 会校验每个路径是否位于 `test\sandbox` 之下，
  **越界立即拒绝并抛错**；所有生成规则、创建测试树、调用 CLI 均先过此校验。
- 测试结束（`run_all_tests.ps1`）自动清理 `test\sandbox`。

### 覆盖内容

CLI 套件覆盖：scan / migrate / restore / change-target / restore-excluded / adopt /
rebuild / delete-rule / import / 占用检测 / 配置防篡改（HMAC 签名）等；
GUI 套件覆盖：主窗口创建与标题、页签控件、规则列表（ListView）、按钮数量、关闭最小化到托盘等。
综合套件覆盖：基本迁移、白名单、恢复、多规则、配置校验拒绝、目标已存在跳过、
占用检测、原子性回滚、配置三处保存、更换目标目录、白名单还原、签名与篡改、扫描接管、
外部链接提醒、新机器重建链接、配置分发、无效规则跳过、导入、贴近实际的测试目录全流程。

---

## 打包为 MSI 安装包

使用 **WiX Toolset v5**（免费）生成单文件 MSI 安装程序：

```powershell
# 1. 安装 WiX 全局工具（一次即可）
dotnet tool install --global wix --version 5.0.2

# 2. 构建 MSI（会自动先构建主程序）
powershell -ExecutionPolicy Bypass -File installer\build_installer.ps1
```

产物：`installer\dist\dirMover-1.0.0.msi`

安装包特性：

- 安装到 `%ProgramFiles%\dirMover`（需要管理员权限，因创建符号链接/安装服务）
- 安装 `dirMover.exe`、`dirMover_cli.exe`，并部署默认 `dirmv.ini`（已存在则不覆盖）
- 创建开始菜单快捷方式
- 卸载时自动停止并删除 `dirMoverSvc` 服务

> 安装后仍可用主程序界面内的「安装服务」按钮注册开机自启动服务。

---

## 使用

### 图形界面（分页：规则 / 计划 / 关于）

以**管理员身份**运行 `dirMover.exe`：

**规则页** —— 规则列表

列表列：**☑（多选） | 监视目录 | 目标目录 | 白名单数 | 状态 | 操作**

- **列表上方**：新建规则… / 导入规则…（与选中行无关）
- **列表下方第一行「规则编辑」**：启用 / 停用 / 删除规则（作用于规则本身）
- **列表下方第二行「任务操作」**：扫描已有迁移… / 重建链接… / 开始迁移 / 恢复 / 停止（作用于选中/勾选规则）
- **多选与全选**：每行第一列有复选框用于**多选**；点击**表头第一列**可**全选 / 全不选**。启用 / 停用 对「勾选的行」生效（未勾选时对选中行生效）
- **行内编辑**：每行「操作」列有一个「**编辑**」按钮；**双击规则行**也可打开编辑窗口
- **新建规则…**：弹出**规则编辑窗口**（监视目录 / 目标目录 / 双面板白名单）→ 点「保存」即添加并启用
- **编辑**：弹出同一窗口；**监视目录只读不可改**；改**目标目录**会询问是否把旧目标数据整体搬迁并重建链接；**新增**白名单项若原本已迁移会自动还原
- **启用规则**：启用时对每条规则做**可用性校验**——不可用的规则**不会启用**（给出提示），且**不影响其他规则**
- **导入规则…**：从某个目录（监视目录或目标目录）的 `.dirmv\dirmv.ini` 导入规则，见「配置文件」章节
- **扫描已有迁移…（接管）**：检查监视目录下的**链接**与目标目录下的**同名数据文件夹**，把**此前人工或其他工具迁移过、但本应用没有记录**的目录补进规则清单；同时**提醒**指向「非本规则目标目录」的链接
- **白名单双面板**：左侧为「非白名单（会迁移）」，列出监视目录下的一级子目录与链接；右侧为「白名单（不迁移）」；**点击任一条目即移动到另一侧**
- **保存与关闭提示**：编辑窗口有「**保存** / **取消**」按钮；若存在未保存修改，点「取消」或直接关闭窗口时会弹出「**有未保存的修改，是否保存？**」
- **删除规则**：先**还原**该规则全部已迁移目录，**全部还原成功才删除**；个别失败则**跳过继续**，完成后把规则标记为「**待删除（还原失败）**」并保留，再次点击删除仅重试失败的目录
- **开始迁移**：只迁移**当前选中**的、启用的、配置正确可用的那一条规则

**计划页** —— 定时、线程数、链接类型、移动模式、实时监视、占用检测与重试

- **定时间隔**选择后只显示对应的编辑项：选「每天」显示「时刻」；选「每周」显示「时刻 + **周几**（下拉）」；选「每月」显示「时刻 + 每月几号」；选「每N天」显示「时刻 + 每几天」；选「不定时」则都不显示
- 只有「**已启用的规则**」才会被**自动迁移、定时迁移与实时监视（hook）**使用

> **自动迁移与定时迁移**作用于**全部「正确可用」的规则**：逐条跳过不可用规则（如监视目录不存在、目标目录为空），互不阻断；
> 文件系统监视（hook）只为正确可用的规则建立监视器，配置变更后自动重建。

**关于页** —— 版本信息与后台服务管理（安装 / 启动 / 停止 / 卸载）

> 规则只能通过上述按钮/弹出窗口操作，**不支持手工编辑配置文件**（配置文件带签名，手改会触发防篡改告警）。

关闭窗口会最小化到系统托盘。

### 命令行

```bat
dirMover_cli.exe scan                                    列出待迁移目录
dirMover_cli.exe migrate                                 执行一次迁移
dirMover_cli.exe restore                                 从日志恢复
dirMover_cli.exe change-target --index 0 --newtarget D:\New   更换某规则目标目录
dirMover_cli.exe restore-excluded --index 0 --name "Data"     还原目录（加入白名单前）
dirMover_cli.exe adopt --index 0                              扫描接管已有迁移/链接
dirMover_cli.exe rebuild --index 0 --watch "C:\New\Data"      新机器上重建链接
dirMover_cli.exe import --dir "D:\DataStore" [--watch C:\New]  从目录配置导入规则
dirMover_cli.exe delete-rule --index 0                    删除规则(先还原再删除)
dirMover_cli.exe install / uninstall                     安装 / 卸载服务（需管理员）
dirMover_cli.exe start / stop / status                   服务控制
           --config <path>                               指定配置文件
```

---

## 配置文件（保存于三处，防篡改）

配置会**同时保存到三个位置**（UTF-8 无 BOM，均带 **HMAC-SHA256 签名**）：

| #   | 位置               | 路径                                                          | 内容                 |
| --- | ------------------ | ------------------------------------------------------------- | -------------------- |
| 1   | 应用程序配置目录   | `%ProgramData%\dirMover\dirmv.ini`（不可写时回退到 exe 目录） | **全部规则**         |
| 2   | 每条规则的监视目录 | `<监视目录>\.dirmv\dirmv.ini`                                 | 仅**与之相关**的规则 |
| 3   | 每条规则的目标目录 | `<目标目录>\.dirmv\dirmv.ini`                                 | 仅**与之相关**的规则 |

> 「与之相关」= 该目录作为规则的**监视目录或目标目录**出现的全部规则
> （例如某目录既是规则 A 的监视目录、又是规则 B 的目标目录，则两份规则都会写入该目录）。

**防篡改**：每个配置文件末尾附带 `#SIG:<64位十六进制>` 行（HMAC-SHA256 签名）。
任何手工改动都会导致签名不匹配，程序加载时会记录并提示「配置文件签名不匹配（可能被篡改）」。

### 迁移到新机器（复制目标目录即可）

把**目标目录**整体复制到新机器后，无需手工重建配置：

1. 新机器上打开 dirMover，目标目录填为复制过来的数据目录
2. 点击规则页的 **「重建链接…」**，选择**新的监视目录**（如用户名不同则为新的 `C:\Users\新用户名\...`）
3. 程序读取目标目录中的配置/清单，在指定位置逐个重建链接指向目标目录

> 也可用命令行：`dirMover_cli.exe rebuild --index 0 --watch "C:\Users\NewUser\Data" --config <ini>`

### 接管已有迁移 / 外部链接提醒

如果之前手工迁移过（监视目录里是链接、目标目录里有文件夹）但应用没有记录：

- 点击 **「扫描接管」**（或 `dirMover_cli.exe adopt --index 0`）自动扫描并补全配置
- 每条规则记录其**全部子目录清单**（`Migrated=` 行，标记出已迁移项）
- 若发现某链接**指向的不是本规则的目标目录**（可能是其他工具或规则创建的），会**弹窗提醒**

配置格式（**白名单为每条规则独立，无全局白名单**）：

```ini
[Rule1]
WatchDir=E:\Data
TargetDir=D:\DataStore
Exclude=AppData|Desktop|Documents|Downloads|...
Enabled=1

[Rule2]
WatchDir=F:\Work
TargetDir=G:\WorkStore
Exclude=Temp|Cache
Enabled=1

[Migrate]
LinkType=0           ; 0=符号链接(需管理员) 1=目录联接
MoveMode=1           ; 1=移动 0=复制保留
SkipExisting=1       ; 目标已存在同名目录时跳过
WorkerThreads=4      ; 线程数 1-64
EnableWatcher=1      ; 启用实时监视

[Schedule]
Period=0             ; 0=不定时 1=每天 2=每周 3=每月 4=每N天
Hour=2               ; 定时时刻(小时)
Minute=0             ; 定时时刻(分钟)
WeekdayMask=0        ; 周模式 bit0=周日..bit6=周六
DayOfMonth=1         ; 月模式
EveryNDays=3         ; 每N天模式

[Lock]
Detect=1             ; 启用占用检测
RetryDelayMinutes=5  ; 占用后重试延迟(分钟)，仅一次
Notify=1

[Service]
ServiceName=dirMoverSvc
ServiceThreads=4

[Log]
LogToFile=1
LogLevel=1
```

- 首次运行会生成一条默认规则：监视目录 = 当前用户目录（`%USERPROFILE%`）、**不启用**、无目标目录、带一组默认白名单。
- 迁移日志：`<目标目录>\link_restore_log.txt`（恢复依据）

---

## 服务器/客户端架构

```text
┌─────────────────┐        命名管道          ┌──────────────────────┐
│  dirMover.exe   │  \\.\pipe\dirMoverControl │  dirMover.exe        │
│  （GUI 前台）    │ ────────────────────────▶ │  （--service 后台）   │
└─────────────────┘   迁移/恢复/状态/停止      └──────────────────────┘
                                                     │
                            定时扫描 + ReadDirectoryChangesW 实时监视
                                                     ▼
                                          迁移非白名单目录并创建链接
```

服务未运行时，GUI 直接在前台执行迁移；服务已运行时，GUI 通过管道委托后台执行。

---

## 目录结构

```text
dirmv/
├── CMakeLists.txt
├── build.bat                     一键构建
├── 迁移软件数据.bat               原始批处理脚本（保留参考）
├── assets/dirmv.ico              应用图标
└── src/
    ├── main.cpp                  wWinMain：GUI / --service / --cli 分派
    ├── cli_main.cpp              独立控制台入口
    ├── resource.rc               图标与版本信息
    ├── core/                     核心库 dirmv_core
    │   ├── Log.*                 日志
    │   ├── Config.*              配置读写与校验
    │   ├── PathUtil.*            路径工具
    │   ├── FileEngine.*          文件系统 API 封装（复制/移动/链接）
    │   ├── MigrateEngine.*       迁移/恢复编排（多线程 + 进度 + ETA）
    │   ├── DirectoryWatcher.*    文件系统实时监视
    │   ├── PipeServer.*          命名管道服务端/客户端
    │   ├── ServiceInstaller.*    服务安装/卸载/启停
    │   └── CliRunner.*           命令行实现
    ├── gui/                      Win32 图形界面
    │   ├── App.*                 应用入口
    │   ├── MainWindow.*          主窗口
    │   ├── ProgressDialog.*      进度对话框
    │   └── TrayIcon.*            系统托盘
    └── service/                  Windows 服务
        ├── ServiceMain.*         SCM 入口与控制处理
        └── ServiceWorker.*       常驻工作循环
```

---

## 注意事项

- **创建符号链接需要管理员权限**（或启用开发者模式）；目录联接（Junction）无需特殊权限但仅限本机 NTFS。
- 迁移采用「复制 + 校验 + 删除源」的移动语义，**跨卷安全**。
- 目标目录已存在同名目录时默认跳过，避免覆盖已有数据。
- 白名单包含强制项（`Windows`、`Program Files`、`ProgramData` 等），不可移除。
- 监视目录与目标目录不能相同或相互包含。
