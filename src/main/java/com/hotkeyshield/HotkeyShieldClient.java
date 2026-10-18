package com.hotkeyshield;

import net.fabricmc.api.ClientModInitializer;
import net.fabricmc.fabric.api.client.command.v2.ClientCommandRegistrationCallback;
import net.fabricmc.fabric.api.client.command.v2.ClientCommands;
import net.fabricmc.fabric.api.client.command.v2.FabricClientCommandSource;
import net.fabricmc.fabric.api.client.event.lifecycle.v1.ClientTickEvents;
import net.minecraft.network.chat.Component;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

/**
 * HotkeyShield 客户端入口。
 *
 * <p>职责：在游戏启动后安装低级键盘钩子，每帧驱动消息泵，
 * 并在退出时卸载钩子（避免影响桌面其它程序）。</p>
 */
public class HotkeyShieldClient implements ClientModInitializer {
    public static final String MOD_ID = "hotkeyshield";
    public static final Logger LOGGER = LoggerFactory.getLogger(MOD_ID);

    /** 当前是否处于屏蔽状态（用户可开关） */
    private static boolean enabled = false;
    private static boolean available = false;
    /** 钩子当前是否真的挂在系统上（跟随窗口焦点） */
    private static boolean hookActive = false;
    /** 焦点去抖：上一次看到的焦点状态与连续 tick 数 */
    private static boolean focusCandidate = false;
    private static int focusStableTicks = 0;
    /** 是否打开按键追踪（诊断用） */
    private static boolean traceOn = false;

    @Override
    public void onInitializeClient() {
        available = WinKeyHook.ensureLoaded();
        if (!available) {
            LOGGER.warn("[HotkeyShield] 原生库加载失败，热键屏蔽功能不可用: {}", WinKeyHook.getLoadError());
        } else {
            LOGGER.info("[HotkeyShield] 原生库已加载，热键屏蔽就绪");
            // 游戏一启动就装钩子，默认屏蔽（用户可用 /hotkeyshield off 关闭）
            setEnabled(true);
        }

        // 按窗口焦点自动挂载/卸载钩子，避免退到桌面后 Win 键失效。
        // 注意：鼠标被锁定时部分环境会上报焦点抖动，这里加去抖，避免来回装卸。
        ClientTickEvents.END_CLIENT_TICK.register(client -> {
            if (!available || !enabled) {
                return;
            }
            boolean focused = client.isWindowActive();
            if (focused == focusCandidate) {
                focusStableTicks++;
            } else {
                focusCandidate = focused;
                focusStableTicks = 0;
            }
            // 连续 10 tick（约 0.5 秒）状态不变才真正切换，避免抖动
            if (focusStableTicks < 10) {
                return;
            }
            if (focusCandidate && !hookActive) {
                WinKeyHook.install();
                hookActive = true;
                LOGGER.info("[HotkeyShield] 钩子已安装");
            } else if (!focusCandidate && hookActive) {
                WinKeyHook.uninstall();
                hookActive = false;
                LOGGER.info("[HotkeyShield] 钩子已卸载");
            }
        });

        // 退出时卸载钩子，并补发 key-up，避免 Alt/Win 卡在按下状态
        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            if (available && enabled) {
                WinKeyHook.uninstall();
            }
        }, "HotkeyShield-Shutdown"));

        registerCommand();
    }

    private void registerCommand() {
        ClientCommandRegistrationCallback.EVENT.register((dispatcher, registryAccess) -> {
            dispatcher.register(ClientCommands.literal("hotkeyshield")
                    .then(ClientCommands.literal("on").executes(ctx -> {
                        setEnabled(true);
                        feedback(ctx.getSource(), "已开启热键屏蔽");
                        return 1;
                    }))
                    .then(ClientCommands.literal("off").executes(ctx -> {
                        setEnabled(false);
                        feedback(ctx.getSource(), "已关闭热键屏蔽");
                        return 1;
                    }))
                    .then(ClientCommands.literal("status").executes(ctx -> {
                        if (!available) {
                            feedback(ctx.getSource(), "原生库不可用: " + WinKeyHook.getLoadError());
                        } else {
                            long total = WinKeyHook.getTotalEvents();
                            long blocked = WinKeyHook.getBlockedCount();
                            feedback(ctx.getSource(), "状态: " + (enabled ? "开启" : "关闭")
                                    + "，钩子收到按键 " + total + " 次，已拦截 " + blocked + " 次，"
                                    + "钩子已安装=" + WinKeyHook.isInstalled());
                            if (total == 0) {
                                feedback(ctx.getSource(), "⚠ 钩子一次按键都没收到，说明钩子没生效");
                            }
                        }
                        return 1;
                    }))
                    .then(ClientCommands.literal("trace").executes(ctx -> {
                        traceOn = !traceOn;
                        WinKeyHook.setTrace(traceOn);
                        feedback(ctx.getSource(), "按键追踪已" + (traceOn ? "开启" : "关闭")
                                + "（开启后按下的键会写进游戏日志）");
                        return 1;
                    })));
        });
    }

    private static void feedback(FabricClientCommandSource source, String msg) {
        source.sendFeedback(Component.literal("[HotkeyShield] " + msg));
    }

    /** 开启/关闭屏蔽。实际挂钩子由焦点检测统一处理，这里只切换开关。 */
    public static void setEnabled(boolean on) {
        if (!available) {
            return;
        }
        enabled = on;
        if (!on) {
            WinKeyHook.uninstall();
            hookActive = false;
        }
        LOGGER.info("[HotkeyShield] 热键屏蔽已{}", on ? "开启" : "关闭");
    }

    public static boolean isEnabled() {
        return enabled;
    }

    public static boolean isAvailable() {
        return available;
    }
}
