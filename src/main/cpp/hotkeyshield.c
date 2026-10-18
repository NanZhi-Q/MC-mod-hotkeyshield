/*
 * HotkeyShield - Windows 底层键盘钩子 (WH_KEYBOARD_LL)
 *
 * 在游戏进程内安装一个低级键盘钩子，吞掉系统级热键，让它们不再被
 * Windows / 输入法处理，从而把按键"还给"游戏。
 */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>

#include <stdint.h>
#include <string.h>

#include "com_hotkeyshield_WinKeyHook.h"

/* ---------------------------------------------------------------- 常量 */

#define MAX_HELD_KEYS 64

/* 我们希望吞掉的虚拟键码 */
#define VK_LWIN_ 0x5B
#define VK_RWIN_ 0x5C
#define VK_LSHIFT_ 0xA0
#define VK_RSHIFT_ 0xA1
#define VK_LCONTROL_ 0xA2
#define VK_RCONTROL_ 0xA3
#define VK_LMENU_ 0xA4
#define VK_RMENU_ 0xA5
#define VK_SPACE_ 0x20
#define VK_TAB_ 0x09
#define VK_ESCAPE_ 0x1B
#define VK_SNAPSHOT_ 0x2C
#define VK_OEM_3_ 0xC0   /* ` / ~ 键：Alt+` 切换输入法 */
#define VK_CONTROL_ 0x11  /* 通用 Ctrl */
#define VK_SHIFT_   0x10  /* 通用 Shift */
#define VK_MENU_    0x12  /* 通用 Alt */

/* 输入法相关：VK_KANA(0x15) / VK_HANJA(0x19) / VK_PROCESSKEY */
#define VK_KANA_ 0x15
#define VK_HANJA_ 0x19
#define VK_PROCESSKEY_ 0xE5

/* ---------------------------------------------------------------- 状态 */

static HHOOK g_hook = NULL;
static DWORD g_hookThreadId = 0;
/* 钩子是否已在后台线程装好（0=未就绪，1=已就绪） */
static volatile LONG g_hookReady = 0;
/* 是否已有线程在跑（防止重复创建线程） */
static volatile LONG g_threadRunning = 0;

/* 由于钩子回调里不能随意做复杂操作，这里用最朴素的方式记录被我们吞掉的键，
 * 用于在钩子关闭/卸载时补发 key-up，避免系统一直认为某个键还按着。 */
static volatile LONG g_heldKeys[MAX_HELD_KEYS];
static volatile LONG g_heldCount = 0;

static volatile LONG64 g_blockedCount = 0;
static volatile LONG g_lastBlockedVk = 0;
/* 钩子收到的按键事件总数（无论是否拦截），用于诊断钩子是否真的在工作 */
static volatile LONG64 g_totalEvents = 0;

/* 是否屏蔽 Win 键（可扩展为配置项，目前默认全部屏蔽） */
static volatile LONG g_blockWin = 1;
/* 是否处理 Ctrl+Space
 *
 * 默认开启。Windows 的 Ctrl+空格 是输入法中英文切换热键，由 TSF/IMM 层处理，
 * 不经过 RegisterHotKey（所以用注册测试查不出来）。它会把空格当成切换指令
 * 吃掉，导致"按住 Ctrl 疾跑时按空格跳不起来"。
 *
 * 本 Mod 的处理不是简单吞掉，而是：拦下 Ctrl+空格 → 先松开 Ctrl →
 * 补发一个干净的空格 → 再把 Ctrl 按回去。这样输入法不切换，游戏照常跳跃。 */
static volatile LONG g_blockIme = 1;
/* 是否屏蔽 Alt+Tab / Alt+Esc */
static volatile LONG g_blockAltTab = 1;
/* 是否屏蔽 Ctrl+Esc（开始菜单） */
static volatile LONG g_blockCtrlEsc = 1;
/* 是否屏蔽 PrtScn */
static volatile LONG g_blockPrintScreen = 1;
/* 诊断：是否把收到的每个按键记录到文件（默认开启，方便排查） */
static volatile LONG g_trace = 1;
/* trace 输出文件 */
static FILE* g_traceFile = NULL;

static void trace_open(void) {
    if (g_traceFile != NULL) {
        return;
    }
    /* 写到系统临时目录，避免权限问题 */
    char path[MAX_PATH];
    char tmp[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, tmp);
    if (n > 0 && n < MAX_PATH) {
        wsprintfA(path, "%shotkeyshield-trace.log", tmp);
        g_traceFile = fopen(path, "a");
    }
}

static void trace_line(const char* fmt, unsigned vk, const char* dir, int ctrl) {
    if (g_traceFile == NULL || InterlockedCompareExchange(&g_trace, 0, 0) == 0) {
        return;
    }
    fprintf(g_traceFile, fmt, vk, dir, ctrl);
    fflush(g_traceFile);
}

/* ---------------------------------------------------------------- 工具 */

/*
 * 钩子自己维护的"当前按下的键"位图。
 *
 * 不能用 GetAsyncKeyState 判断 Ctrl/Alt 是否按下：低级键盘钩子的回调发生在
 * 系统把事件并入异步状态表之前，此时 Ctrl 的按下状态可能还没更新，导致
 * "Ctrl+Space" 里读不到 ctrl，空格就漏过去了。这里在回调里自己记账。
 */
static volatile LONG64 g_downBits[4];   /* 256 个虚拟键的位图 */

static void mark_down(int vk) {
    if (vk >= 0 && vk < 256) {
        InterlockedOr64(&g_downBits[vk >> 6], (LONG64) 1 << (vk & 63));
    }
}

static void mark_up(int vk) {
    if (vk >= 0 && vk < 256) {
        InterlockedAnd64(&g_downBits[vk >> 6], ~((LONG64) 1 << (vk & 63)));
    }
}

static int is_down_bit(int vk) {
    if (vk < 0 || vk >= 256) {
        return 0;
    }
    return (InterlockedCompareExchange64(&g_downBits[vk >> 6], 0, 0) >> (vk & 63) & 1) != 0;
}

static int ctrl_down(void) {
    return is_down_bit(VK_LCONTROL_) || is_down_bit(VK_RCONTROL_)
            || is_down_bit(VK_CONTROL_);
}

static int alt_down(void) {
    return is_down_bit(VK_LMENU_) || is_down_bit(VK_RMENU_)
            || is_down_bit(VK_MENU_);
}

static int shift_down(void) {
    return is_down_bit(VK_LSHIFT_) || is_down_bit(VK_RSHIFT_)
            || is_down_bit(VK_SHIFT_);
}

static void push_held(int vk) {
    int i;
    LONG count = InterlockedCompareExchange(&g_heldCount, 0, 0);
    for (i = 0; i < count; i++) {
        if (g_heldKeys[i] == vk) {
            return;
        }
    }
    if (count < MAX_HELD_KEYS) {
        g_heldKeys[count] = vk;
        InterlockedIncrement(&g_heldCount);
    }
}

static void remove_held(int vk) {
    LONG count = InterlockedCompareExchange(&g_heldCount, 0, 0);
    LONG i;
    for (i = 0; i < count; i++) {
        if (g_heldKeys[i] == vk) {
            LONG j;
            for (j = i; j < count - 1; j++) {
                g_heldKeys[j] = g_heldKeys[j + 1];
            }
            g_heldKeys[count - 1] = 0;
            InterlockedDecrement(&g_heldCount);
            return;
        }
    }
}

/* 把被吞掉的键补发一次 key-up，防止 Alt/Win 等在系统层面"卡住" */
static void release_all_held(void) {
    LONG count = InterlockedExchange(&g_heldCount, 0);
    LONG i;
    for (i = 0; i < count; i++) {
        int vk = (int) g_heldKeys[i];
        if (vk == 0) {
            continue;
        }
        g_heldKeys[i] = 0;
        {
            INPUT in;
            memset(&in, 0, sizeof(in));
            in.type = INPUT_KEYBOARD;
            in.ki.wVk = (WORD) vk;
            in.ki.wScan = (WORD) MapVirtualKeyW((UINT) vk, MAPVK_VK_TO_VSC);
            in.ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(1, &in, sizeof(INPUT));
        }
    }
}

static int should_block(int vk, int isDown) {
    int ctrl = ctrl_down();
    int alt = alt_down();

    /* Win 键以及所有 Win+X 组合 */
    if (InterlockedCompareExchange(&g_blockWin, 0, 0) && (vk == VK_LWIN_ || vk == VK_RWIN_)) {
        return 1;
    }

    /* 输入法热键：kanji 键、IME 处理键、Alt+` */
    if (InterlockedCompareExchange(&g_blockIme, 0, 0)) {
        if (vk == VK_KANA_ || vk == VK_HANJA_ || vk == VK_PROCESSKEY_) {
            return 1;
        }
        /* Alt+` 切换输入法 */
        if (vk == VK_OEM_3_ && alt) {
            return 1;
        }
    }

    if (InterlockedCompareExchange(&g_blockAltTab, 0, 0)) {
        if ((vk == VK_TAB_ || vk == VK_ESCAPE_) && alt) {
            return 1;
        }
    }

    if (InterlockedCompareExchange(&g_blockCtrlEsc, 0, 0)) {
        if (vk == VK_ESCAPE_ && ctrl) {
            return 1;
        }
    }

    if (InterlockedCompareExchange(&g_blockPrintScreen, 0, 0)) {
        if (vk == VK_SNAPSHOT_) {
            return 1;
        }
    }

    (void) isDown;
    return 0;
}

/* ---------------------------------------------------------------- 钩子回调 */

static LRESULT CALLBACK low_level_keyboard_proc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* kb = (KBDLLHOOKSTRUCT*) lParam;
        int isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        int isUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
        int vk = (int) kb->vkCode;
        int injected = (kb->flags & LLKHF_INJECTED) != 0;

        /* 先记账：无论是否拦截，都要维护按下状态位图，供 should_block 判断修饰键 */
        if (!injected && (isDown || isUp)) {
            InterlockedIncrement64(&g_totalEvents);
            if (isDown) {
                mark_down(vk);
            } else {
                mark_up(vk);
            }
        }

        /* 特殊处理：Ctrl + 空格
         *
         * 这是 Windows 输入法中英文切换的默认热键，由 TSF/IMM 层处理，
         * 不经过 RegisterHotKey，所以注册测试查不出来。它会把"空格"当成
         * 切换指令吃掉，导致游戏收不到跳跃键。
         *
         * 处理方式：吞掉原始的空格事件（阻止输入法切换），
         * 然后补发"先松开 Ctrl → 空格按下/抬起 → 再按回 Ctrl"这一串事件，
         * 让游戏以一个干净的空格收到跳跃。
         */
        if (!injected && (isDown || isUp) && vk == VK_SPACE_ && ctrl_down()
                && InterlockedCompareExchange(&g_blockIme, 0, 0)) {
            INPUT seq[5];
            int n = 0;
            int ctrlVk = is_down_bit(VK_LCONTROL_) ? VK_LCONTROL_ : VK_RCONTROL_;
            int ctrlWasDown = is_down_bit(ctrlVk);

            InterlockedIncrement64(&g_blockedCount);
            InterlockedExchange(&g_lastBlockedVk, vk);
            trace_line("REMAP   vk=0x%02X %s ctrl=%d (Ctrl+Space -> Space)\n",
                       vk, isDown ? "DOWN" : "UP", ctrl_down());

            memset(seq, 0, sizeof(seq));
            /* 1) 先松开 Ctrl，让游戏看到的是纯空格 */
            if (ctrlWasDown) {
                seq[n].type = INPUT_KEYBOARD;
                seq[n].ki.wVk = (WORD) ctrlVk;
                seq[n].ki.dwFlags = KEYEVENTF_KEYUP;
                n++;
            }
            /* 2) 补发空格 */
            seq[n].type = INPUT_KEYBOARD;
            seq[n].ki.wVk = VK_SPACE_;
            seq[n].ki.wScan = (WORD) MapVirtualKeyW(VK_SPACE_, MAPVK_VK_TO_VSC);
            seq[n].ki.dwFlags = isDown ? 0 : KEYEVENTF_KEYUP;
            n++;
            /* 3) 空格抬起后再把 Ctrl 按回去，保持疾跑 */
            if (ctrlWasDown) {
                seq[n].type = INPUT_KEYBOARD;
                seq[n].ki.wVk = (WORD) ctrlVk;
                seq[n].ki.dwFlags = 0;
                n++;
            }
            SendInput((UINT) n, seq, sizeof(INPUT));
            /* 截断原始事件，输入法切换不会发生 */
            return 1;
        }

        if (!injected && (isDown || isUp) && should_block(vk, isDown)) {
            if (isDown) {
                push_held(vk);
                InterlockedIncrement64(&g_blockedCount);
                InterlockedExchange(&g_lastBlockedVk, vk);
            } else {
                remove_held(vk);
            }
            if (InterlockedCompareExchange(&g_trace, 0, 0)) {
                trace_line("BLOCKED vk=0x%02X %s ctrl=%d\n", vk, isDown ? "DOWN" : "UP", ctrl_down());
            }
            /* 返回 1 = 吞掉该按键，不再传递给系统 */
            return 1;
        }

        if (!injected && (isDown || isUp) && InterlockedCompareExchange(&g_trace, 0, 0)) {
            trace_line("pass    vk=0x%02X %s ctrl=%d\n", vk, isDown ? "DOWN" : "UP", ctrl_down());
        }
    }
    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

/* ---------------------------------------------------------------- JNI */

/* 钩子线程：在这个线程里建消息循环，钩子回调由系统投递到这里执行 */
static DWORD WINAPI hook_thread_proc(LPVOID param) {
    MSG msg;
    DWORD myId = GetCurrentThreadId();
    (void) param;

    /* 先建立消息队列，再装钩子 */
    PeekMessageW(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);
    g_hookThreadId = myId;
    trace_open();
    if (g_traceFile != NULL) {
        fprintf(g_traceFile, "===== hook install thread=%lu =====\n", (unsigned long)myId);
        fflush(g_traceFile);
    }
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, low_level_keyboard_proc,
                               GetModuleHandleW(NULL), 0);
    if (g_hook == NULL) {
        g_hookThreadId = 0;   /* 让下一次 install 能重试 */
        return 1;
    }
    InterlockedExchange(&g_hookReady, 1);

    /* 阻塞式消息循环：钩子回调依赖它 */
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_hook != NULL) {
        UnhookWindowsHookEx(g_hook);
        g_hook = NULL;
    }
    /* 线程即将退出：清标记，允许以后重新创建 */
    InterlockedExchange(&g_threadRunning, 0);
    g_hookThreadId = 0;
    InterlockedExchange(&g_hookReady, 0);
    return 0;
}

JNIEXPORT jboolean JNICALL Java_com_hotkeyshield_WinKeyHook_install(JNIEnv* env, jclass cls) {
    (void) env;
    (void) cls;
    /* 钩子已装好 */
    if (g_hook != NULL) {
        return JNI_TRUE;
    }
    /* 已有线程在跑（可能刚创建、还没装好），不重复创建 */
    if (InterlockedCompareExchange(&g_threadRunning, 1, 0) != 0) {
        return JNI_TRUE;
    }
    InterlockedExchange(&g_hookReady, 0);

    /* 不能阻塞渲染线程，也不该在渲染线程装钩子：
     * 低级钩子回调只会在"安装它的线程"的消息循环里执行，
     * 而渲染线程的消息循环归 GLFW 所有，我们抢不到，钩子会失效。
     * 所以直接开一个专用线程，异步完成安装，本函数立即返回。 */
    {
        HANDLE th = CreateThread(NULL, 0, hook_thread_proc, NULL, 0, &g_hookThreadId);
        if (th == NULL) {
            InterlockedExchange(&g_threadRunning, 0);
            g_hookThreadId = 0;
            return JNI_FALSE;
        }
        CloseHandle(th);
    }
    return JNI_TRUE;
}

JNIEXPORT void JNICALL Java_com_hotkeyshield_WinKeyHook_uninstall(JNIEnv* env, jclass cls) {
    (void) env;
    (void) cls;
    if (InterlockedCompareExchange(&g_threadRunning, 1, 0) != 0) {
        DWORD tid = g_hookThreadId;
        if (tid != 0) {
            PostThreadMessageW(tid, WM_QUIT, 0, 0);
        }
        {
            int waited = 0;
            while (InterlockedCompareExchange(&g_threadRunning, 0, 0) != 0 && waited < 500) {
                Sleep(2);
                waited += 2;
            }
        }
    }
    if (g_hook != NULL) {
        /* 兜底：线程没能自己清理 */
        UnhookWindowsHookEx(g_hook);
        g_hook = NULL;
    }
    InterlockedExchange(&g_threadRunning, 0);
    g_hookThreadId = 0;
    InterlockedExchange(&g_hookReady, 0);
    /* 清空按下状态位图，避免下次安装时残留 */
    InterlockedExchange64(&g_downBits[0], 0);
    InterlockedExchange64(&g_downBits[1], 0);
    InterlockedExchange64(&g_downBits[2], 0);
    InterlockedExchange64(&g_downBits[3], 0);
    release_all_held();
}

JNIEXPORT jboolean JNICALL Java_com_hotkeyshield_WinKeyHook_isInstalled(JNIEnv* env, jclass cls) {
    (void) env;
    (void) cls;
    return g_hook != NULL ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_hotkeyshield_WinKeyHook_pumpMessages(JNIEnv* env, jclass cls) {
    /* 钩子现在跑在专用线程里，自带消息循环，这里无需也无法代它泵消息。
     * 保留这个方法是为了兼容旧调用点。 */
    (void) env;
    (void) cls;
}

JNIEXPORT jlong JNICALL Java_com_hotkeyshield_WinKeyHook_getBlockedCount(JNIEnv* env, jclass cls) {
    (void) env;
    (void) cls;
    return (jlong) InterlockedCompareExchange64(&g_blockedCount, 0, 0);
}

/* 诊断用：钩子收到的按键事件总数 */
JNIEXPORT jlong JNICALL Java_com_hotkeyshield_WinKeyHook_getTotalEvents(JNIEnv* env, jclass cls) {
    (void) env;
    (void) cls;
    return (jlong) InterlockedCompareExchange64(&g_totalEvents, 0, 0);
}

JNIEXPORT jint JNICALL Java_com_hotkeyshield_WinKeyHook_getLastBlockedVk(JNIEnv* env, jclass cls) {
    (void) env;
    (void) cls;
    return (jint) InterlockedCompareExchange(&g_lastBlockedVk, 0, 0);
}

JNIEXPORT void JNICALL Java_com_hotkeyshield_WinKeyHook_resetCounters(JNIEnv* env, jclass cls) {
    (void) env;
    (void) cls;
    InterlockedExchange64(&g_blockedCount, 0);
    InterlockedExchange(&g_lastBlockedVk, 0);
}

/* 诊断开关 */
JNIEXPORT void JNICALL Java_com_hotkeyshield_WinKeyHook_setTrace(JNIEnv* env, jclass cls, jboolean on) {
    (void) env;
    (void) cls;
    InterlockedExchange(&g_trace, on ? 1 : 0);
    if (on) {
        trace_open();
    }
    if (g_traceFile != NULL) {
        fprintf(g_traceFile, "--- trace %s ---\n", on ? "ON" : "OFF");
        fflush(g_traceFile);
    }
}

/* 供 Java 侧按需调整屏蔽项（预留） */
JNIEXPORT void JNICALL Java_com_hotkeyshield_WinKeyHook_setOptions(JNIEnv* env, jclass cls,
                                                                  jboolean win, jboolean ime,
                                                                  jboolean altTab, jboolean ctrlEsc,
                                                                  jboolean printScreen) {
    (void) env;
    (void) cls;
    InterlockedExchange(&g_blockWin, win ? 1 : 0);
    InterlockedExchange(&g_blockIme, ime ? 1 : 0);
    InterlockedExchange(&g_blockAltTab, altTab ? 1 : 0);
    InterlockedExchange(&g_blockCtrlEsc, ctrlEsc ? 1 : 0);
    InterlockedExchange(&g_blockPrintScreen, printScreen ? 1 : 0);
}
