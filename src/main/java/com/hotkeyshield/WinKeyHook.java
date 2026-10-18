package com.hotkeyshield;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;

/**
 * Windows 底层键盘钩子（WH_KEYBOARD_LL）的 JNI 绑定。
 *
 * <p>系统热键（Ctrl+空格 输入法切换、Win 键、Alt+Tab、Ctrl+Esc 等）在 Windows 的
 * 键盘消息队列之前就被处理掉了，Minecraft/Java 层的按键事件根本收不到。
 * 唯一能在游戏进程内拦截它们的办法就是装一个低级键盘钩子。本类通过 JNI
 * 调用 Windows API 完成这件事。</p>
 *
 * <p>钩子回调运行在安装它的线程消息循环中，所以必须在主渲染线程调用
 * {@link #install()} / {@link #uninstall()}，并由 {@link HotkeyShieldClient}
 * 每帧调用 {@link #pumpMessages()} 来驱动消息分发。</p>
 */
public final class WinKeyHook {
    private WinKeyHook() {
    }

    private static boolean loaded = false;
    private static String loadError = null;

    /** 尝试加载本机库；返回是否成功。失败原因可用 {@link #getLoadError()} 查看。 */
    public static synchronized boolean ensureLoaded() {
        if (loaded) {
            return true;
        }
        if (loadError != null) {
            return false;
        }
        String os = System.getProperty("os.name", "").toLowerCase();
        String arch = System.getProperty("os.arch", "").toLowerCase();
        if (!os.contains("windows")) {
            loadError = "非 Windows 系统，热键屏蔽不可用";
            return false;
        }
        if (!arch.contains("64")) {
            loadError = "仅支持 64 位 Windows";
            return false;
        }
        try {
            // jar 内的 DLL 不能直接 loadLibrary，必须先解压到临时目录
            File dll = extractDll();
            System.load(dll.getAbsolutePath());
            loaded = true;
            return true;
        } catch (Throwable t) {
            loadError = "无法加载 native 库 hotkeyshield: " + t;
            return false;
        }
    }

    /** 把 jar 里的 hotkeyshield.dll 解压到临时目录并返回该文件。 */
    private static File extractDll() throws IOException {
        try (InputStream in = WinKeyHook.class.getResourceAsStream("/hotkeyshield.dll")) {
            if (in == null) {
                throw new IOException("jar 中缺少 hotkeyshield.dll");
            }
            File dir = new File(System.getProperty("java.io.tmpdir"), "hotkeyshield");
            if (!dir.exists() && !dir.mkdirs()) {
                throw new IOException("无法创建临时目录: " + dir);
            }
            File dll = new File(dir, "hotkeyshield.dll");
            if (dll.exists() && !dll.delete()) {
                // 可能被占用，换一个名字
                dll = new File(dir, "hotkeyshield_" + System.nanoTime() + ".dll");
            }
            Files.copy(in, dll.toPath(), StandardCopyOption.REPLACE_EXISTING);
            dll.deleteOnExit();
            return dll;
        }
    }

    public static synchronized String getLoadError() {
        return loadError;
    }

    public static boolean isLoaded() {
        return loaded;
    }

    /** 安装低级键盘钩子。必须在拥有消息队列的 UI 线程调用。 */
    public static native boolean install();

    /** 卸载钩子并恢复被屏蔽的按键状态。 */
    public static native void uninstall();

    /** 钩子是否处于启用（屏蔽）状态。 */
    public static native boolean isInstalled();

    /** 派发本线程待处理的 Win32 消息；每个渲染帧调用一次即可。 */
    public static native void pumpMessages();

    /** 被本 mod 吞掉的按键次数（用于 HUD/调试显示）。 */
    public static native long getBlockedCount();

    /** 诊断用：钩子收到的按键事件总数。若这个值一直是 0，说明钩子没在工作。 */
    public static native long getTotalEvents();

    /** 上一次被吞掉的键：虚拟键码（0 表示无）。 */
    public static native int getLastBlockedVk();

    /** 本次会话累计拦截次数统计清零。 */
    public static native void resetCounters();

    /** 按需调整具体屏蔽项（默认全部开启）。 */
    public static native void setOptions(boolean win, boolean ime, boolean altTab,
                                         boolean ctrlEsc, boolean printScreen);

    /** 诊断：把钩子收到的每个按键打印到游戏日志。 */
    public static native void setTrace(boolean on);
}
