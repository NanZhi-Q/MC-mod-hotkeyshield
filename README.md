<div align="center">

# HotkeyShield · 热键锁

**按住 Ctrl 疾跑时按空格跳不起来？**

拦截 Windows 全局热键，把按键还给游戏。

[![Minecraft](https://img.shields.io/badge/Minecraft-26.3-brightgreen)]()
[![Fabric](https://img.shields.io/badge/Fabric-0.19.5+-blue)]()
[![Java](https://img.shields.io/badge/Java-25-orange)]()
[![License](https://img.shields.io/badge/License-MIT-lightgrey)]()

</div>

---

## 📖 简介

hotkeyshield 是一个 **minecraft26.3 Fabric 客户端 Mod**，通过 Windows 低级键盘钩子（`WH_KEYBOARD_LL`）拦截系统级快捷键，让你的游戏按键不再被操作系统或输入法抢走。

它解决的是一个很具体、也很常见的痛点：系统热键会占用快捷键

> 按住 `Ctrl` 疾跑时按 `空格`，角色**跳不起来**。

原因通常不是键盘坏了，也不是游戏 Bug，而是 **Windows 的 `Ctrl + 空格` 是输入法中英文切换热键**——它由 TSF/IMM 层处理，把空格当成"切换输入法"的指令吃掉了，游戏根本收不到这个按键。

Khotkeyshield 的处理不是简单吞掉按键（那只会让空格彻底消失），而是：

```
原始事件: Ctrl↓  Space↓
              ↓
         拦截，走重映射
              ↓
   Ctrl↑ → Space↓ → Space↑ → Ctrl↓
              ↓
   输入法不切换，游戏正常跳跃，疾跑不中断
```

---

## ✨ 功能

| 热键 | 默认状态 | 说明 |
|---|---|---|
| `Ctrl + 空格` | ✅ 重映射 | **核心功能**。拦下输入法切换，补发干净的跳跃键 |
| `Win` 键 | ✅ 屏蔽 | 防止误触弹出开始菜单 |
| `Alt + Tab` / `Alt + Esc` | ✅ 屏蔽 | 防止全屏游戏被切出 |
| `Ctrl + Esc` | ✅ 屏蔽 | 防止误开开始菜单 |
| `PrtScn` | ✅ 屏蔽 | 防止截图弹窗打断游戏 |
| `Alt + ~` | ✅ 屏蔽 | 输入法切换热键 |
| `Kana` / `Hanja` | ✅ 屏蔽 | 日韩输入法相关键 |

> **不会**拦截 `W`、`A`、`S`、`D`、玩家自定义的空格、Shift 等游戏按键本身。

---

## 🎯 智能焦点联动

低级键盘钩子是**全局**的——如果无条件安装，你切到桌面后 `Win` 键也会失效，这会很烦。

hotkeyshield 的处理：

- 游戏窗口**有焦点** → 自动安装钩子
- 切换到桌面/其他程序 → **自动卸载**，`Win` 键立即恢复
- 连续 10 tick（约 0.5 秒）焦点稳定才切换，避免抖动导致反复装卸

---

## 📦 安装

### 环境要求

- Minecraft **26.3**
- Fabric Loader **≥ 0.19.5**
- Java **25**
- **64 位 Windows**

### 步骤

1. 从 [Releases](../../releases) 下载 `hotkeyshield-1.0.0-mc26.3.jar`
2. 放进整合包的 `mods` 文件夹
3. 启动游戏

启动后日志中应出现：

```
[HotkeyShield] 原生库已加载，热键屏蔽就绪
[HotkeyShield] 钩子已安装
```

---

## 🎮 使用

**默认自动开启**，无需任何配置。

游戏内聊天栏可用命令：

| 命令 | 作用 |
|---|---|
| `/hotkeyshield on` | 开启屏蔽 |
| `/hotkeyshield off` | 关闭屏蔽 |
| `/hotkeyshield status` | 查看状态、钩子收到的按键数、拦截次数 |
| `/hotkeyshield trace` | 开启/关闭按键追踪（诊断用） |

> 命令名目前为 `hotkeyshield`（与 Mod ID 一致）。

---

## ✅ 验证是否生效

按顺序测试：

1. **聊天框测试**（最直接）
   按 `T` 打开聊天框 → 按住 `Ctrl` 再按 `空格`
   → **应该能打出空格**
   如果打不出，说明重映射没生效。

2. **疾跑跳跃**
   进世界 → 按住 `Ctrl + W` 疾跑 → 按 `空格`
   → **应该能正常跳跃**

3. **系统隔离**
   切到桌面 → 按 `Win` 键
   → **开始菜单应该正常弹出**（说明焦点联动生效）

---

## 🔧 诊断

如果功能异常，用 `/hotkeyshield trace` 打开追踪，之后每个按键都会记录到：

```
%TEMP%\hotkeyshield-trace.log
```

格式示例：

```
REMAP   vk=0x20 DOWN ctrl=1 (Ctrl+Space -> Space)   ← Ctrl+空格被重映射
BLOCKED vk=0x5B DOWN ctrl=0                          ← Win 键被拦截
pass    vk=0x20 DOWN ctrl=0                          ← 普通空格放行
```

也可以只看 `/hotkeyshield status` 的数字：

- `钩子收到按键 0 次` → 钩子没收到任何按键，检查是否安装成功
- `钩子已安装=false` → 安装失败
- 有数字但拦截为 0 → 钩子在工作，但没有按键命中规则

---

## 🔍 技术原理

### 为什么普通 Mod 做不到？

Minecraft 是 Java 程序，Fabric/Forge 的按键事件监听都在**游戏内部**。而 `Ctrl + 空格` 这类热键在**操作系统层面**就被截获了，游戏的事件回调根本不会被触发。

唯一的办法是在进程内安装 **Win32 低级键盘钩子**。

### 关键实现

| 问题 | 解法 |
|---|---|
| 钩子回调只在"安装它的线程"的消息循环中执行 | 开**专用线程**跑自己的消息循环，不能占用渲染线程（那归 GLFW 所有） |
| `GetAsyncKeyState` 在钩子回调中读不到 Ctrl | 钩子**自己维护按键按下位图** |
| 卸载后 Alt/Win 卡在"按下"状态 | 卸载时补发 key-up |
| `Ctrl + 空格` 直接吞掉会让空格彻底消失 | **重映射**：松开 Ctrl → 发空格 → 按回 Ctrl |
| 渲染线程被阻塞导致卡顿 | 安装过程完全异步 |

### 项目结构

```
hotkey-shield/
├── src/main/
│   ├── java/com/hotkeyshield/
│   │   ├── HotkeyShieldClient.java    # 客户端入口、命令、焦点联动
│   │   └── WinKeyHook.java            # JNI 绑定、DLL 解压加载
│   ├── cpp/
│   │   ├── hotkeyshield.c             # 低级键盘钩子核心实现
│   │   └── com_hotkeyshield_WinKeyHook.h
│   └── resources/
│       ├── fabric.mod.json
│       └── hotkeyshield.dll           # 预编译的 64 位 native 库
├── build.gradle
└── gradle.properties
```

---

## 🛠 从源码构建

### 依赖

- JDK 25
- Gradle 9.7.1
- MSVC（编译 DLL）
- JDK 的 JNI 头文件（`jni.h`、`win32/jni_md.h`）

### 构建 Java 部分

```bash
./gradlew build
```

产物在 `build/libs/hotkeyshield-1.0.0.jar`。

### 编译原生 DLL

```bat
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cl /nologo /LD /O2 /I"<jni头文件目录>" /I"<jni头文件目录>\win32" ^
   src\main\cpp\hotkeyshield.c /link user32.lib ^
   /Fe:src\main\resources\hotkeyshield.dll
```

> DLL 必须在 `gradle build` **之前**编译好，否则不会被打进 jar。

### 重新生成 JNI 头文件

```bash
javac -h src/main/cpp src/main/java/com/hotkeyshield/WinKeyHook.java
```

---

## ⚠️ 已知限制

- **仅支持 64 位 Windows**，仅支持 Minecraft **26.3**
- 低级键盘钩子只对**物理键鼠**生效；部分键盘驱动的硬件级宏可能绕过
- 部分杀毒软件会对"键盘钩子"行为报警，属正常现象，加白名单即可
- 某些录屏/远程桌面软件自身的输入处理不受影响

---

## ❓ FAQ

<details>
<summary><b>为什么不用简单的做法，直接把 Ctrl+空格 删掉？</b></summary>

那样空格会被彻底吞掉，游戏同样收不到跳跃键——只是把"被输入法抢走"换成了"被 Mod 抢走"。必须重映射才能两者兼得。
</details>

<details>
<summary><b>会影响输入法正常使用吗？</b></summary>

不会。只在 `Ctrl + 空格` 这个组合上做重映射，正常打字、`Shift` 切换中英文都不受影响。
</details>

<details>
<summary><b>和 IMBlocker 冲突吗？</b></summary>

不冲突。两者工作层面不同：IMBlocker 处理输入法焦点/候选框，KeyGuard 处理系统热键拦截。可以同时使用。
</details>

<details>
<summary><b>为什么切到桌面后 Win 键又正常了？</b></summary>

设计如此。钩子只在游戏窗口有焦点时生效，避免影响日常使用。
</details>

---

## 📄 许可证

[MIT](LICENSE)

## 🙏 致谢

- [Fabric](https://fabricmc.net/) — Mod 加载器
- 灵感来自无数个"Ctrl+空格 跳不起来"的求助帖
