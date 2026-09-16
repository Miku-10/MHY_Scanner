# MHY_Scanner

<div align="center">

[![GitHub stars](https://img.shields.io/github/stars/Theresa-0328/MHY_Scanner?color=blue&style=for-the-badge)](https://github.com/Theresa-0328/MHY_Scanner/stargazers)
</div>

### **版本 - v1.20.0**

## 说明
本项目为免费开源项目，用于学习和研究，禁止商业化用途。

## 功能和特性
- 从屏幕自动获取二维码登录，适用于大部分登录情景，不适用于在竞争激烈时抢码。
- 从直播流获取二维码登录，适用于抢码登录情景。
- 可选启动后自动开始识别屏幕和识别完成后自动退出，无需登录后手动切窗口关闭。
- 表格化管理多账号，方便切换游戏账号。

## 目前可用的平台
|   崩坏3    | 原神  | 星穹铁道 | 绝区零 |
| :--------: | :---: | :------:| :----: |
|    官服    | 官服  |   官服   |  官服  |
| BiliBili |       |         |        |

## 使用说明
[点击Releases](https://github.com/Theresa-0328/MHY_Scanner/releases) 选择最新版本下载解压

[点击下载安装Visual C++ 运行时库](https://aka.ms/vs/17/release/vc_redist.x64.exe)，详细解释查看[Microsoft官方文档](https://learn.microsoft.com/zh-cn/cpp/windows/latest-supported-vc-redist?view=msvc-170)。

运行 MHY_Scanner.exe

点击菜单栏 **账号管理->添加账号**，添加你的账号。

双击账号对应的备注单元格可以添加自定义备注。

登陆后点击 **监视屏幕** 就可以自动识别任意显示在屏幕上的二维码并自动登录。

选择你需要的直播平台,在当前直播间输入框输入`RID`，点击 **监视直播间** 就可以自动识别该直播间显示的二维码并自动登录。

正在执行的任务的按钮会高亮显示，再次点击会停止。

`RID`是纯数字，一般从直播间链接中获得。

|                平台                |           `<RID>` 位置            |
| :--------------------------------: | :-------------------------------: |
| [B 站](https://live.bilibili.com/) | `https://live.bilibili.com/<RID>` |
|  [抖音](https://live.douyin.com/)  |  `https://live.douyin.com/<RID>`  |

目前没有进行大量测试，如果有任何建议和问题欢迎提Issues。

## v1.20.0 更新说明
相对早期版本，本分支在尽量不改动核心扫码协议的前提下，做了以下修复与体验优化，欢迎审阅与反馈：

### 功能修复
- **屏幕监视**：修复 DXGI staging 纹理参数非法导致「能采到帧但画面全透明、二维码无反应」的问题；按 `RowPitch` 逐行拷贝，适配多显卡输出选择。
- **直播间监视**：修复拉流初始化失败后界面仍显示「监视中」的静默问题；增大 `probesize`、优先选取 avc 流，并补充 B 站 Referer/UA。
- **账号登录**：扫码登录与 stoken 校验链路按现行 passport 接口对齐，修复「监视前提示登录状态失效」。

### 界面
- 主界面改为卡片式浅色布局，窗口可缩放。
- 提示/确认弹窗统一为中文按钮与一致样式。
- 顶栏增加就绪 / 监视中状态指示。

### 其他
- 关键模块补充中文注释，便于后续维护。
- 需要管理员权限运行（桌面复制 API 要求）。

**说明**：本分支不含任何账号 Cookie / token；用户配置目录 `Config/` 已在 `.gitignore` 中忽略。

## 编译
请参考CI/CD工作流

## 相关项目
- [KuRo_Scanner](https://github.com/Theresa-0328/KuRo_Scanner) – 鸣潮扫码器

## 参考和感谢
- [HonkaiScanner](https://github.com/HonkaiScanner)

- [BililiveRecorder/BililiveRecorder](https://github.com/BililiveRecorder/BililiveRecorder)

- [DGP-Studio/Snap.Hutao](https://github.com/DGP-Studio/Snap.Hutao)

感谢原作者 [@Theresa-0328](https://github.com/Theresa-0328) 开源本项目。
