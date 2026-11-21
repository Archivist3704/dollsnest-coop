# Dolls Nest 联机 mod

《Dolls Nest》的双人（最多四人）合作 mod。主机开着世界，客机的机体出现在主机那里：你们能看见对方
在走动、开枪，敌人的血量和生死两边一致。

mod 本体是一个原生 DLL 加一个很小的加载器。**不修改任何游戏文件、不需要注入器、不需要管理员权限。**

> **状态**：v685，在 2 台和 3 台机器上实测通过（游戏 1.3.2）。本仓库就是那一版的源码。

---

## 给玩家

### 安装

1. 把发布包的 zip 解压到你的 Steam 库目录：

   ```
   <Steam 库>\steamapps\common\Dolls Nest\
   ```

   压缩包里已经有 `DollsNest\Binaries\Win64\` 这一层，解压到那里就等于把 `version.dll` 和
   `dollsnest-coop.dll` 放到了游戏找的位置。

2. 像平常一样从 Steam 启动游戏。不要用注入器，也不要改启动项。

3. 确认游戏目录里出现了 `dsh-proxy-load.txt`（时间就是刚才），并且 `dollsnest-coop.log` 的第一屏
   有 `build=` 和 `file sha256=`。

### 联机

1. 双方都装 Radmin VPN（或同类虚拟局域网工具），进同一个网络。
2. 在游戏的 `Win64` 目录里建一个 `peers.txt`，写对方的虚拟局域网地址，一行一个；**双方都写对方**。
   没有这个文件时走局域网广播。
3. 主机先进地图，按 **F7**，选第一行。
4. 客机在几秒内按 **F7**，选第二行。
5. 两个人必须在同一张地图上 —— 不同地图互相看不见，这是设计。

菜单跟着游戏语言走（简体中文 / 繁體中文 / 日本語 / English）。

### 已知限制

- 只有同图才能配对；换地图会结束会话。
- 在休息点补给、或者死亡，都会结束会话 —— 之后重新按 F7。
- 主机结束会话时客机不会收到提示，需要自己退出。
- 实测过 2 人和 3 人，第 4 个位置没有测过。
- 当客机时建议用一份存档副本 —— 游戏的自动存档会写进它读的那个档。

---

## 给开发者

### 构建

需要 Visual Studio 2022/2026 的生成工具（MSVC 19.5x）和 CMake 3.2x 以上。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build\build.ps1 -BootstrapName dollsnest-coop-v685
```

产物是 `out\bin\dollsnest-coop-v685.dll`（附带 `.pdb`）。**脚本成功时退出码也是 1**，判据是输出里
的 `[build] OK`。

离线测试套件会一起编出来：

```powershell
out\bin\dsh-selftest.exe
```

必须打印 `checks: 734 failures: 0`。

### 目录

| 位置 | 是什么 |
|---|---|
| `Code/dsh-bootstrap` | mod 怎么进游戏：加载器、命令通道、F7 菜单、房间发现 |
| `Code/dsh-game` | 会话本体：发什么、造什么、镜像什么 |
| `Code/dsh-reflect` | 读写游戏里的 actor —— 唯一知道游戏字段布局的地方 |
| `Code/dsh-net` | UDP 传输、会话握手、报文外壳 |
| `Code/dsh-sync` | 插值、回放钟、发送节奏 |
| `Code/dsh-platform` | 日志、hook 包装、文件哈希 |
| `Code/dsh-selftest` | 离线套件：不需要游戏的那部分 |
| `Code/dsh-proxy` | 冒充 `version.dll` 的转发壳，让游戏自己加载 mod |
| `tools/build`、`tools/deploy` | 构建、部署到游戏目录、出发布包 |

### 它大概怎么工作

主机每秒二十次采样自己机体的位置和外观，通过一套很小的 UDP 协议发出去；客机把外观拼起来，在自己的
世界里造出同一台机体，并让它保持同步。敌人的血量、任务状态、子弹和道具使用都走同一条路。主机是世界
的权威；客机报告自己那侧发生了什么，由主机裁决。

### 说明

- `Code/third_party/minhook` 是随仓库携带的第三方库（BSD-3-Clause），也是唯一的第三方依赖。
- `Code/third_party/layout` 不随仓库发布：偏移编在代码里，运行时自检。
- 两台机器必须是同一个构建：日志第一屏的 `file sha256=` 要一样。

---

## 与官方无关

这是玩家自制的 mod，与游戏的开发者、发行方没有任何关系，也没有得到他们的认可。
