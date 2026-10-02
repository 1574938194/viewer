# Viewer

Viewer 的缩写为 **VR**，程序图标使用 VR 字母与播放符号。

Viewer 是面向 Windows 的原生本地资源库，命令行入口为 `vr.exe`（或
`vr`）。当前第一版管理视频和图片，并把正式导入的文件归档到 Managed
Storage，再通过 SQLite 元数据和缩略图网格快速访问。

## 当前界面

主窗口固定为三段式布局：

1. 标题栏：Viewer 标题、最小化、最大化/还原、关闭。
2. 导航栏：搜索框和综合排序（最近打开、热度、最近添加、创建时间、文件大小）。
3. 工作区：左侧资源库/Tag 筛选，右侧图片和视频缩略图网格。

网格支持虚拟化绘制、键盘选择、Ctrl/Shift 多选、双击打开、右键菜单和
鼠标滚轮。视频缩略图右下角显示半透明 `mm:ss` 时长徽章（分钟数可超过 59）；
导入时优先使用 FFmpeg 的 `ffprobe` 读取时长，不可用时回退到 Windows 属性系统。
缩略图在后台通过 WIC 或
Windows Shell 生成，并使用内存 LRU 和磁盘缓存。

## 核心工作流

拖入单个媒体文件或包含媒体的文件夹后，Viewer 会递归发现 `.mp4`、`.mkv`、
`.avi`、`.wmv`、`.mov`、`.m4v`、`.webm`、`.ts`、`.jpg`、`.jpeg`、`.png`、
`.webp`、`.bmp`，然后：

```text
校验 → Import Dialog → Managed Storage 归档 → 校验 → SQLite 提交
     → 按设置移入回收站删除来源 → 异步生成缩略图
```

同盘优先 Move；跨盘使用 `.incoming` 临时目录、复制校验和原子改名。任何
失败都会尽量保留来源文件。Delete 默认移入回收站，`Shift+Delete` 为永久
删除并要求确认。

导入对话框支持批量文件、标题、Category、Tag、Actor 和 Favorite。Tag
别名、全文检索（SQLite FTS5）、最近打开、热度、收藏、缺失状态和操作日志
均保存在数据库中。文件夹导入时，所拖入文件夹的名称会自动作为该文件夹内
所有媒体的集体 Tag（可在导入后继续编辑）。

## 命令行

```text
vr                         打开资源库
vr 白丝 3p                 启动并搜索关键词
vr "白丝 3p 内射"          启动并搜索完整短语
vr --help                  显示帮助
vr --db-info               显示数据库统计
vr --backup                备份 nexus.db
vr --scan                  扫描 Managed Storage 并恢复索引
vr --recover               扫描并统计 .incoming 临时文件
vr --export                导出 Metadata CSV
```

搜索支持 `type:video`、`type:image`、`category:...`、`tag:...`、
`actor:...`、`favorite:true`、`opened:<30d`、`duration:>60m`、`+词` 和
`-词` 等条件。FTS5 是运行时必需组件。

## 数据目录

为兼容已有资源库，改名后继续使用 `nexus.toml`、`data/nexus.db` 和
`NexusLibrary`。默认使用程序目录下的便携式布局：

```text
vr.exe
nexus.toml
data/nexus.db
cache/thumbnails/
NexusLibrary/video/00/...
NexusLibrary/image/00/...
NexusLibrary/.incoming/
```

可在设置中指定独立的托管库目录；留空时使用上述默认目录。播放器和图片
查看器留空时使用 Windows 文件关联。设置还可以关闭“导入成功后删除来源”
和“拖放文件/文件夹时询问 Metadata”，并在“预设标签”列表中添加、删除
导入/编辑 Metadata 时显示的快捷标签（默认：白丝、美少女、萝莉、3p、肛交、内射）。

## 构建与测试

要求 Visual Studio 2022、CMake/Ninja，以及带 FTS5 的 vcpkg SQLite3
（默认路径 `D:/vcpkg/installed/x64-windows`）。

```cmd
build.bat
ctest --test-dir build --output-on-failure
```

集成测试覆盖 SQLite 迁移、归档回滚、哈希去重、搜索、文件夹递归导入与集体
Tag、批量删除、缺失扫描和数据库备份。
