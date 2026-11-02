// main.cpp - OpenLasso window: layout, tray icon, menus, graph (GDI+), process tree, rules, options, log, service mode (/service).
#include "process_supervisor.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winhttp.h>
#include <gdiplus.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <functional>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "pdh.lib")
#endif

static const wchar_t* kAppVersion = L"1.0";
static const int GRAPH_H = 140;
static const int BOTTOM_H = 270;
static const int STATUS_H = 22;
static const int HIST_MAX = 480;          // 480 samples x 250 ms = 2 minutes
static const UINT WM_TRAY = WM_APP + 1;
enum { TIMER_SAMPLE = 1, TIMER_PROCS = 2 };
enum { SAMPLE_MS = 250, PROCS_MS = 1000 };

enum { IDC_THREADLIST = 200, IDC_TH_HINT };
enum {
    IDM_CTX_ALLGROUPS = 4390,
    IDM_TH_PRIO = 4400,                                  // + 0..6
    IDM_TH_AFF = 4410,                                   // + 0..2
    IDM_TH_BOOST_ON = 4420, IDM_TH_BOOST_OFF, IDM_TH_SUSPEND, IDM_TH_RESUME
};
enum {
    IDM_CTX_MATCH_NAME = 4300, IDM_CTX_MATCH_FOLDER, IDM_CTX_MATCH_PATH, IDM_CTX_MATCH_DEFAULT,
    IDM_CTX_LIMIT25 = 4310, IDM_CTX_LIMIT50, IDM_CTX_LIMIT_OFF,
    IDM_CTX_WD_LOW = 4320, IDM_CTX_WD_KILL, IDM_CTX_WD_OFF,
    IDM_CTX_BLOCK = 4330, IDM_CTX_BALANCE,
    IDM_VIEW_REPORT = 3360,
    IDM_OPT_NOTIFY = 3530, IDM_OPT_DELEGATE = 3531,
    IDM_OPT_THROTTLESEC = 3540                                  // + 0..3 = 1, 2, 5, 10 seconds
};
enum {
    IDC_VTAB = 100, IDC_FILTER, IDC_PROCLIST, IDC_PAUSE, IDC_EDITRULES, IDC_HIDEGRAPH, IDC_SUMMARY, IDC_VIEWLOG,
    IDC_LOGFILTER, IDC_BTAB, IDC_LOG, IDC_LOG_CLEAR, IDC_RULELIST, IDC_DEL_RULE, IDC_RUN_RULES, IDC_EXPORT,
    IDC_IMPORT, IDC_SAVE, IDC_STATUS, IDC_OPT_THROTTLE, IDC_OPT_THRESH, IDC_LBL_THRESH, IDC_OPT_PROC, IDC_LBL_PROC,
    IDC_OPT_BOOST, IDC_OPT_AUTO, IDC_OPT_AUTOSTART, IDC_OPT_FORCED, IDC_OPT_SMART, IDC_OPT_IDLE, IDC_OPT_PERF,
    IDC_OPT_TRAY, IDC_OPT_EXCL, IDC_LBL_EXCL, IDC_LBL_SVC, IDC_SVC_INST, IDC_SVC_UNINST, IDC_SVC_START, IDC_SVC_STOP,
    IDC_OPT_GPUFB, IDC_OPT_PERFPLAN
};
enum {
    IDM_MAIN_HIDE = 3000, IDM_MAIN_ALLUSERS, IDM_MAIN_ELEVATE, IDM_MAIN_THROTTLE, IDM_MAIN_SMART, IDM_MAIN_IDLE,
    IDM_MAIN_PERF, IDM_MAIN_ENGINE, IDM_MAIN_EXIT,
    IDM_FILE_SAVE = 3010, IDM_FILE_NEWPROFILE, IDM_FILE_IMPORT_RULES, IDM_FILE_EXPORT_RULES, IDM_FILE_IMPORT_ALL,
    IDM_FILE_EXPORT_ALL, IDM_FILE_EDIT, IDM_FILE_LOGFOLDER,
    IDM_PROFILE_BASE = 3100,                   // + index (0 = default)
    IDM_PLAN_BASE = 3200,                      // + index into the power plan list
    IDM_AWAKE_OFF = 3300, IDM_AWAKE_1H, IDM_AWAKE_2H, IDM_AWAKE_4H, IDM_AWAKE_INF,
    IDM_VIEW_LOG = 3320, IDM_VIEW_SUMMARY, IDM_VIEW_PAUSERC, IDM_VIEW_CPUUTIL, IDM_VIEW_GRAPH,
    IDM_GC_EVENTS = 3330, IDM_GC_CPULINE, IDM_GC_CPUMETER, IDM_GC_RAMMETER, IDM_GC_PLAN,
    IDM_GC_RAMLINE = 3336, IDM_GC_GPULINE, IDM_GC_GPUMETER,
    IDM_LOGSIZE_BASE = 3380,                   // + 0..3 (see kLogSizesKB)
    IDM_PERF_CREATE = 3520,
    IDM_TRAY_ALWAYS = 3340, IDM_TRAY_MIN, IDM_TRAY_NEVER,
    IDM_THEME_LIGHT = 3345, IDM_THEME_DARK, IDM_VIEW_SMOOTH,
    IDM_RESET_COLS = 3350, IDM_RESET_VIEW,
    IDM_VIEW_TREE = 3355, IDM_VIEW_GRID, IDM_VIEW_ICONS, IDM_VIEW_TIPS, IDM_VIEW_ONTOP,
    IDM_COL_BASE = 3400,                       // + column id
    IDM_OPT_STARTUP = 3500, IDM_OPT_STARTMIN, IDM_OPT_CLOSETRAY, IDM_OPT_LOG, IDM_OPT_LOGCLEAR, IDM_OPT_BOOST,
    IDM_OPT_FREEMEM, IDM_OPT_FORCED, IDM_OPT_SETTINGS, IDM_OPT_RULES, IDM_OPT_RESET,
    IDM_TOOL_CFG, IDM_TOOL_LOG, IDM_TOOL_TM, IDM_TOOL_SVC,
    IDM_UPD_CHECK = 3600, IDM_UPD_NEVER, IDM_UPD_DAILY, IDM_UPD_WEEKLY, IDM_UPD_BETA, IDM_UPD_AUTO,
    IDM_HELP_DOC = 3700, IDM_HELP_WEB, IDM_HELP_DIAG, IDM_ABOUT,
    IDM_TRAY_SHOW = 3710, IDM_TRAY_PAUSE,
    IDM_CTX_PRIO = 4000, IDM_CTX_PRIO_ALW = 4010,      // + index
    IDM_CTX_AFF = 4020, IDM_CTX_AFF_ALW = 4030,        // + 0 all, 1 first half, 2 second half
    IDM_CTX_MEM = 4040, IDM_CTX_MEM_ALW = 4050,        // + 1..5
    IDM_CTX_ECO_ON = 4060, IDM_CTX_ECO_OFF, IDM_CTX_ECO_ON_ALW, IDM_CTX_ECO_OFF_ALW,
    IDM_CTX_IO = 4070, IDM_CTX_IO_ALW = 4080,          // + 0..3
    IDM_CTX_TB_ON = 4090, IDM_CTX_TB_OFF, IDM_CTX_TB_ON_ALW, IDM_CTX_TB_OFF_ALW,
    IDM_CTX_EXCL = 4100, IDM_CTX_FREEMEM, IDM_CTX_CLOSE, IDM_CTX_TERM, IDM_CTX_LOCATE,
    IDM_CTX_COPY_NAME, IDM_CTX_COPY_PATH, IDM_CTX_COPY_PID, IDM_CTX_COPY_RULES, IDM_CTX_PASTE_RULES,
    IDM_CTX_REMOVE_RULES, IDM_CTX_RESTART, IDM_CTX_PLAN_NONE = 4199,
    IDM_CTX_PLAN = 4200                                 // + index
};
enum {
    IDM_CTX_CPUSET = 4120, IDM_CTX_CPUSET_ALW = 4130,        // + 1..3
    IDM_CTX_GPUPRIO = 4140, IDM_CTX_GPUPRIO_ALW = 4150,      // + 1..6 (D3DKMT class + 1)
    IDM_CTX_GPUPREF = 4160, IDM_CTX_GPUPREF_ALW = 4170,      // + 0..2
    IDM_CTX_SLEEP_ON = 4180, IDM_CTX_SLEEP_OFF, IDM_CTX_SLEEP_ON_ALW, IDM_CTX_SLEEP_OFF_ALW,
    IDM_CTX_PERF_ON = 4184, IDM_CTX_PERF_OFF, IDM_CTX_PERF_ON_ALW, IDM_CTX_PERF_OFF_ALW,
    IDM_CTX_RESTART_ADMIN = 4188,
    IDM_CTX_KEEP_ON = 4190, IDM_CTX_KEEP_ON_ELEV, IDM_CTX_KEEP_CMD, IDM_CTX_KEEP_OFF
};
static const int kLogSizesKB[4] = { 256, 1024, 4096, 16384 };

enum { BOTTOM_LOG, BOTTOM_RULES, BOTTOM_OPTIONS, BOTTOM_THREADS, BOTTOM_COUNT };
enum { TAB_ALL, TAB_ACTIVE };
enum { C_NAME, C_USER, C_PID, C_STATUS, C_RULES, C_PRIO, C_AFF, C_CPU, C_THREADS, C_PB, C_MEM, C_ECO, C_IO, C_GPU, C_GPUMEM, C_RAM, C_COUNT };

static const wchar_t* kColTitle[C_COUNT] = { L"Process name", L"User", L"PID", L"Status", L"Rules", L"Priority class",
    L"CPU affinity", L"CPU (%)", L"Threads", L"Throttle time", L"Memory priority", L"Efficiency", L"I/O priority", L"GPU (%)", L"GPU memory (MB)", L"RAM (MB)" };
static const int kColWidth[C_COUNT] = { 230, 110, 60, 80, 50, 120, 90, 70, 60, 110, 110, 80, 90, 80, 120, 90 };

// controls shown for each bottom tab
static const int kLogIds[] = { IDC_LOG, IDC_LOG_CLEAR };
static const int kThreadIds[] = { IDC_THREADLIST, IDC_TH_HINT };
static HWND g_thList = nullptr;
static std::vector<ThreadInfo> g_threads;
static const int kThPrio[7] = { -15, -2, -1, 0, 1, 2, 15 };
static const int kRuleIds[] = { IDC_RULELIST, IDC_DEL_RULE, IDC_RUN_RULES, IDC_EXPORT, IDC_IMPORT };
static const int kOptIds[] = { IDC_OPT_THROTTLE, IDC_OPT_THRESH, IDC_LBL_THRESH, IDC_OPT_PROC, IDC_LBL_PROC, IDC_OPT_BOOST,
    IDC_OPT_AUTO, IDC_OPT_AUTOSTART, IDC_OPT_FORCED, IDC_OPT_SMART, IDC_OPT_IDLE, IDC_OPT_PERF, IDC_OPT_TRAY,
    IDC_OPT_EXCL, IDC_LBL_EXCL, IDC_LBL_SVC, IDC_SVC_INST, IDC_SVC_UNINST, IDC_SVC_START, IDC_SVC_STOP,
    IDC_OPT_GPUFB, IDC_OPT_PERFPLAN };

// ---------- globals ----------
static ProcessSupervisor g_sv;
static HWND g_main, g_vtab, g_filter, g_proc, g_pause, g_editRules, g_hideGraph, g_btab, g_log, g_rule, g_status;
static std::vector<ProcessInfo> g_procs;
static std::vector<PowerPlanInfo> g_plans;
static std::vector<int> g_vis;
static std::vector<double> g_hist, g_histRam, g_histGpu;
static std::vector<bool> g_evt;
static bool g_evtPending = false;
static bool g_paused = false, g_showGraph = true, g_activeOnly = false, g_startMinimized = false, g_startupOn = false;
static bool g_trayAdded = false;
static NOTIFYICONDATAW g_nid = {};
static int g_bottomTab = BOTTOM_LOG;
static std::wstring g_msg = L"Ready.";
static std::wstring g_cpuName, g_curProfile;
static HFONT g_titleFont, g_smallFont;
static HIMAGELIST g_imgList = nullptr;
static std::map<std::wstring, int> g_iconCache;
static HBRUSH g_darkBrush = nullptr;
static HMENU g_menuProfiles = nullptr, g_menuPlansMain = nullptr, g_menuPlansOpt = nullptr, g_menuColumns = nullptr;
static HMENU g_menuAwake = nullptr, g_menuAwake2 = nullptr;
static const wchar_t* kMemShort[] = { L"-", L"Very low", L"Low", L"Medium", L"Below normal", L"Normal" };

// ---------- helpers ----------
static std::wstring GetWText(HWND h) {
    int n = GetWindowTextLengthW(h);
    if (n <= 0) return L"";
    std::vector<wchar_t> b(n + 1, 0);
    GetWindowTextW(h, b.data(), n + 1);
    return std::wstring(b.data());
}
static std::wstring LowerW(std::wstring s) {
    for (auto& ch : s) ch = (wchar_t)towlower(ch);
    return s;
}
static std::wstring StripText(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}
static std::wstring Utf8ToW(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::wstring AffToText(DWORD_PTR m) {
    if (!m) return L"-";
    wchar_t b[32];
    swprintf_s(b, L"0x%llX", (unsigned long long)m);
    return b;
}
static std::wstring MemShortOf(DWORD v) { return (v >= 1 && v <= 5) ? kMemShort[v] : L"-"; }
static std::wstring PlanName(const std::wstring& guid) {
    if (guid.empty()) return L"-";
    for (const auto& p : g_plans)
        if (_wcsicmp(p.guid.c_str(), guid.c_str()) == 0) return p.name;
    return guid;
}
static void SetSub(HWND lv, int row, int col, const std::wstring& s) {
    ListView_SetItemText(lv, row, col, const_cast<LPWSTR>(s.c_str()));
}
static void AddColumn(HWND lv, int idx, const wchar_t* text, int w) {
    LVCOLUMNW c = {};
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    c.pszText = const_cast<LPWSTR>(text);
    c.cx = w;
    c.iSubItem = idx;
    ListView_InsertColumn(lv, idx, &c);
}
static HWND Make(const wchar_t* cls, const wchar_t* txt, DWORD style, int id, DWORD ex = 0) {
    HWND h = CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g_main,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return h;
}
static void UpdateStatus();
static void SetStatus(const std::wstring& s) {
    g_msg = s;
    if (g_status) UpdateStatus();
}
static DWORD_PTR SystemAffinityMask() {
    DWORD_PTR pm = 0, sm = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm);
    return sm;
}
static DWORD_PTR AffinityPreset(int k) {
    DWORD_PTR sm = SystemAffinityMask();
    if (k == 0) return sm;
    int n = 0;
    for (DWORD_PTR m = sm; m; m &= m - 1) ++n;
    int lowN = n / 2, cnt = 0;
    DWORD_PTR low = 0;
    for (int bit = 0; bit < 64 && cnt < lowN; ++bit)
        if (sm & ((DWORD_PTR)1 << bit)) { low |= (DWORD_PTR)1 << bit; ++cnt; }
    return (k == 1) ? low : (sm & ~low);
}
static bool SetClipboardTextW(const std::wstring& t) {
    if (!OpenClipboard(g_main)) return false;
    EmptyClipboard();
    size_t bytes = (t.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h) { CloseClipboard(); return false; }
    memcpy(GlobalLock(h), t.c_str(), bytes);
    GlobalUnlock(h);
    SetClipboardData(CF_UNICODETEXT, h);
    CloseClipboard();
    return true;
}
static std::wstring GetClipboardTextW() {
    if (!OpenClipboard(g_main)) return L"";
    std::wstring out;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) out = (const wchar_t*)GlobalLock(h), GlobalUnlock(h);
    CloseClipboard();
    return out;
}

// ---------- dark theme: owner-painted tabs, buttons and list headers ----------
// Windows only themes the window frame and scroll bars; tab headers, push buttons, check boxes and list
// headers are painted here when the dark theme is on (a subclass passes everything through otherwise).
static const COLORREF DK_BG = RGB(32, 34, 36), DK_CTL = RGB(52, 55, 59), DK_HOT = RGB(68, 72, 77),
                      DK_PUSH = RGB(84, 90, 97), DK_EDGE = RGB(92, 96, 102), DK_TEXT = RGB(230, 230, 230),
                      DK_DIM = RGB(130, 132, 135), DK_ACCENT = RGB(76, 148, 232);
static bool IsDark() { return g_sv.Cfg().darkTheme; }
static void FillRectColor(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}
static void FrameRectColor(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FrameRect(dc, &r, b);
    DeleteObject(b);
}
static LRESULT CALLBACK DarkButtonProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
    if (m == WM_NCDESTROY) {
        RemovePropW(h, L"olHot");
        RemoveWindowSubclass(h, DarkButtonProc, id);
        return DefSubclassProc(h, m, w, l);
    }
    if (!IsDark()) return DefSubclassProc(h, m, w, l);
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE:
        if (!GetPropW(h, L"olHot")) {
            SetPropW(h, L"olHot", (HANDLE)1);
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
            InvalidateRect(h, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        RemovePropW(h, L"olHot");
        InvalidateRect(h, nullptr, FALSE);
        break;
    case WM_LBUTTONUP: case WM_SETFOCUS: case WM_KILLFOCUS: case WM_ENABLE: case WM_SETTEXT:
    case BM_SETCHECK: case BM_SETSTATE: {
        LRESULT r = DefSubclassProc(h, m, w, l);
        InvalidateRect(h, nullptr, FALSE);
        return r;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        DWORD type = (DWORD)GetWindowLongW(h, GWL_STYLE) & BS_TYPEMASK;
        bool isCheck = (type == BS_CHECKBOX || type == BS_AUTOCHECKBOX);
        LRESULT st = SendMessageW(h, BM_GETSTATE, 0, 0);
        bool pushed = (st & BST_PUSHED) != 0, focus = (st & BST_FOCUS) != 0;
        bool hot = GetPropW(h, L"olHot") != nullptr, off = !IsWindowEnabled(h);
        std::wstring text = GetWText(h);
        FillRectColor(dc, rc, DK_BG);
        HGDIOBJ oldF = SelectObject(dc, (HFONT)SendMessageW(h, WM_GETFONT, 0, 0));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, off ? DK_DIM : DK_TEXT);
        if (isCheck) {
            int bs = 14, by = (rc.top + rc.bottom - bs) / 2;
            RECT box = { rc.left + 2, by, rc.left + 2 + bs, by + bs };
            FillRectColor(dc, box, hot ? DK_HOT : DK_CTL);
            FrameRectColor(dc, box, focus ? DK_ACCENT : DK_EDGE);
            if (SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                HPEN pen = CreatePen(PS_SOLID, 2, off ? DK_DIM : DK_ACCENT);
                HGDIOBJ op = SelectObject(dc, pen);
                MoveToEx(dc, box.left + 3, box.top + 7, nullptr);
                LineTo(dc, box.left + 6, box.top + 10);
                LineTo(dc, box.left + 11, box.top + 4);
                SelectObject(dc, op);
                DeleteObject(pen);
            }
            RECT tr = { box.right + 6, rc.top, rc.right, rc.bottom };
            DrawTextW(dc, text.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
        } else {
            FillRectColor(dc, rc, pushed ? DK_PUSH : hot ? DK_HOT : DK_CTL);
            FrameRectColor(dc, rc, focus ? DK_ACCENT : DK_EDGE);
            DrawTextW(dc, text.c_str(), -1, &rc, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        SelectObject(dc, oldF);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefSubclassProc(h, m, w, l);
}
static LRESULT CALLBACK DarkTabProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
    if (m == WM_NCDESTROY) {
        RemoveWindowSubclass(h, DarkTabProc, id);
        return DefSubclassProc(h, m, w, l);
    }
    if (!IsDark()) return DefSubclassProc(h, m, w, l);
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        FillRectColor(dc, rc, DK_BG);
        HGDIOBJ oldF = SelectObject(dc, (HFONT)SendMessageW(h, WM_GETFONT, 0, 0));
        SetBkMode(dc, TRANSPARENT);
        int n = TabCtrl_GetItemCount(h), cur = TabCtrl_GetCurSel(h);
        for (int i = 0; i < n; ++i) {
            RECT r;
            if (!TabCtrl_GetItemRect(h, i, &r)) continue;
            bool sel = (i == cur);
            FillRectColor(dc, r, sel ? DK_CTL : DK_BG);
            if (sel) {
                RECT line = { r.left, r.bottom - 3, r.right, r.bottom };
                FillRectColor(dc, line, DK_ACCENT);
            }
            wchar_t buf[128] = {};
            TCITEMW ti = {};
            ti.mask = TCIF_TEXT;
            ti.pszText = buf;
            ti.cchTextMax = 127;
            TabCtrl_GetItem(h, i, &ti);
            SetTextColor(dc, sel ? DK_TEXT : DK_DIM);
            DrawTextW(dc, buf, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
        }
        SelectObject(dc, oldF);
        EndPaint(h, &ps);
        return 0;
    }
    return DefSubclassProc(h, m, w, l);
}
static LRESULT CALLBACK DarkHeaderProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
    if (m == WM_NCDESTROY) {
        RemoveWindowSubclass(h, DarkHeaderProc, id);
        return DefSubclassProc(h, m, w, l);
    }
    if (!IsDark()) return DefSubclassProc(h, m, w, l);
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        FillRectColor(dc, rc, DK_CTL);
        HGDIOBJ oldF = SelectObject(dc, (HFONT)SendMessageW(h, WM_GETFONT, 0, 0));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, DK_TEXT);
        int n = Header_GetItemCount(h);
        for (int i = 0; i < n; ++i) {
            RECT r;
            if (!Header_GetItemRect(h, i, &r)) continue;
            wchar_t buf[128] = {};
            HDITEMW hi = {};
            hi.mask = HDI_TEXT;
            hi.pszText = buf;
            hi.cchTextMax = 127;
            Header_GetItem(h, i, &hi);
            RECT edge = { r.right - 1, r.top + 3, r.right, r.bottom - 3 };
            FillRectColor(dc, edge, DK_EDGE);
            RECT tr = { r.left + 6, r.top, r.right - 4, r.bottom };
            DrawTextW(dc, buf, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        RECT bottom = { rc.left, rc.bottom - 1, rc.right, rc.bottom };
        FillRectColor(dc, bottom, DK_EDGE);
        SelectObject(dc, oldF);
        EndPaint(h, &ps);
        return 0;
    }
    return DefSubclassProc(h, m, w, l);
}
static BOOL CALLBACK SubclassEnum(HWND c, LPARAM) {
    wchar_t cls[64] = {};
    GetClassNameW(c, cls, 64);
    if (_wcsicmp(cls, L"Button") == 0) SetWindowSubclass(c, DarkButtonProc, 1, 0);
    else if (_wcsicmp(cls, WC_TABCONTROLW) == 0) SetWindowSubclass(c, DarkTabProc, 2, 0);
    return TRUE;
}
static void InstallDarkSubclasses(HWND parent, std::initializer_list<HWND> lists) {
    EnumChildWindows(parent, SubclassEnum, 0);
    for (HWND lv : lists) {
        HWND hd = ListView_GetHeader(lv);
        if (hd) SetWindowSubclass(hd, DarkHeaderProc, 3, 0);
    }
}
// Dark scroll bars and selection colours (uxtheme) and the dark title bar (dwmapi) are looked up at run time.
static void SetControlTheme(HWND h, bool dark) {
    typedef HRESULT(WINAPI * PFN)(HWND, LPCWSTR, LPCWSTR);
    static PFN fn = []() -> PFN {
        HMODULE m = LoadLibraryW(L"uxtheme.dll");
        return m ? (PFN)GetProcAddress(m, "SetWindowTheme") : nullptr;
    }();
    if (fn) fn(h, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
}
static void SetDarkTitleBar(HWND h, bool dark) {
    typedef HRESULT(WINAPI * PFN)(HWND, DWORD, LPCVOID, DWORD);
    static PFN fn = []() -> PFN {
        HMODULE m = LoadLibraryW(L"dwmapi.dll");
        return m ? (PFN)GetProcAddress(m, "DwmSetWindowAttribute") : nullptr;
    }();
    BOOL v = dark ? TRUE : FALSE;
    if (fn) {
        fn(h, 20, &v, sizeof(v));    // DWMWA_USE_IMMERSIVE_DARK_MODE
        fn(h, 19, &v, sizeof(v));    // same attribute on early Windows 10 builds
    }
}

// ---------- input box (small modal window with a text field) ----------
static bool g_inDone = false, g_inOk = false;
static LRESULT CALLBACK InputProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_COMMAND) {
        if (LOWORD(w) == IDOK) { g_inOk = true; g_inDone = true; }
        if (LOWORD(w) == IDCANCEL) g_inDone = true;
        return 0;
    }
    if (m == WM_CLOSE) { g_inDone = true; return 0; }
    return DefWindowProcW(h, m, w, l);
}
static bool InputBox(const wchar_t* title, const wchar_t* prompt, std::wstring& value) {
    static bool reg = false;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    if (!reg) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = InputProc;
        wc.hInstance = hi;
        wc.lpszClassName = L"OLInputBox";
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        RegisterClassW(&wc);
        reg = true;
    }
    RECT r;
    GetWindowRect(g_main, &r);
    HWND h = CreateWindowExW(0, L"OLInputBox", title, WS_POPUP | WS_CAPTION | WS_SYSMENU,
                             r.left + 120, r.top + 160, 380, 150, g_main, nullptr, hi, nullptr);
    HWND st = CreateWindowW(L"STATIC", prompt, WS_CHILD | WS_VISIBLE, 12, 12, 350, 20, h, nullptr, hi, nullptr);
    HWND e = CreateWindowW(L"EDIT", value.c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 12, 40, 350, 24, h,
                           (HMENU)901, hi, nullptr);
    CreateWindowW(L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 196, 84, 80, 26, h, (HMENU)IDOK, hi, nullptr);
    CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE, 284, 84, 80, 26, h, (HMENU)IDCANCEL, hi, nullptr);
    for (HWND c : { st, e }) SendMessageW(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    EnableWindow(g_main, FALSE);
    ShowWindow(h, SW_SHOW);
    SetFocus(e);
    g_inDone = false;
    g_inOk = false;
    MSG m;
    while (!g_inDone && GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(h, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    if (g_inOk) value = StripText(GetWText(e));
    EnableWindow(g_main, TRUE);
    DestroyWindow(h);
    SetForegroundWindow(g_main);
    return g_inOk;
}

// ---------- selection and rules ----------
static bool CurrentSel(ProcessInfo& out) {
    int sel = ListView_GetNextItem(g_proc, -1, LVNI_SELECTED);
    if (sel < 0) return false;
    LVITEMW it = {};
    it.mask = LVIF_PARAM;
    it.iItem = sel;
    ListView_GetItem(g_proc, &it);
    if (it.lParam < 0 || it.lParam >= (LPARAM)g_procs.size()) return false;
    out = g_procs[(size_t)it.lParam];
    return true;
}
static void SyncRulesFromList() {
    auto& rules = g_sv.Rules();
    for (int i = 0; i < (int)rules.size(); ++i)
        rules[i].enabled = ListView_GetCheckState(g_rule, i) != 0;
}
static const wchar_t* kCpuSetLbl[4] = { L"(unchanged)", L"All CPUs", L"First half", L"Second half" };
static const wchar_t* kGpuPrioLbl[7] = { L"(unchanged)", L"Idle", L"Below normal", L"Normal", L"Above normal", L"High", L"Realtime" };
static const wchar_t* kGpuPrefLbl[4] = { L"(unchanged)", L"Default", L"Power saving", L"High performance" };
static void FillRuleList() {
    auto& rules = g_sv.Rules();
    ListView_DeleteAllItems(g_rule);
    for (int i = 0; i < (int)rules.size(); ++i) {
        const ProcessRule& r = rules[i];
        LVITEMW it = {};
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = const_cast<LPWSTR>(r.processName.c_str());
        ListView_InsertItem(g_rule, &it);
        ListView_SetCheckState(g_rule, i, r.enabled);
        SetSub(g_rule, i, 1, r.priorityClass ? PriorityToLabel(r.priorityClass) : std::wstring(L"(unchanged)"));
        SetSub(g_rule, i, 2, r.affinityMask ? AffToText(r.affinityMask) : std::wstring(L"(unchanged)"));
        SetSub(g_rule, i, 3, r.memoryPriority ? MemShortOf(r.memoryPriority) : std::wstring(L"(unchanged)"));
        SetSub(g_rule, i, 4, r.eco == 1 ? L"On" : r.eco == 2 ? L"Off" : L"(unchanged)");
        SetSub(g_rule, i, 5, PlanName(r.powerPlan));
        SetSub(g_rule, i, 6, kCpuSetLbl[r.cpuSets]);
        SetSub(g_rule, i, 7, kGpuPrioLbl[r.gpuPriority]);
        SetSub(g_rule, i, 8, kGpuPrefLbl[r.gpuPref]);
        std::wstring holds = r.preventSleep ? L"Sleep" : L"";
        if (r.forcePower) holds += holds.empty() ? L"Performance" : L", Performance";
        SetSub(g_rule, i, 9, holds.empty() ? std::wstring(L"-") : holds);
        SetSub(g_rule, i, 10, r.keepRunning ? (r.keepRunningElevate ? L"Yes (admin)" : L"Yes") : L"-");
        SetSub(g_rule, i, 11, r.keepRunning ? (r.keepRunningCommandLine.empty() ? std::wstring(L"(original)") : r.keepRunningCommandLine)
                                            : std::wstring(L"-"));
    }
}
static void SetRuleField(const std::wstring& name, const std::function<void(ProcessRule&)>& fn) {
    SyncRulesFromList();
    auto& v = g_sv.Rules();
    ProcessRule* target = nullptr;
    for (auto& r : v)
        if (_wcsicmp(r.processName.c_str(), name.c_str()) == 0) target = &r;
    if (!target) {
        ProcessRule nr;
        nr.processName = name;
        v.push_back(nr);
        target = &v.back();
    }
    fn(*target);
    target->enabled = true;
    FillRuleList();
}

// ---------- options controls <-> settings ----------
static void SyncOptionsFromControls() {
    Settings& s = g_sv.Cfg();
    s.throttle = IsDlgButtonChecked(g_main, IDC_OPT_THROTTLE) == BST_CHECKED;
    s.boost = IsDlgButtonChecked(g_main, IDC_OPT_BOOST) == BST_CHECKED;
    s.autoApply = IsDlgButtonChecked(g_main, IDC_OPT_AUTO) == BST_CHECKED;
    s.forced = IsDlgButtonChecked(g_main, IDC_OPT_FORCED) == BST_CHECKED;
    s.memoryOptimizer = IsDlgButtonChecked(g_main, IDC_OPT_SMART) == BST_CHECKED;
    s.idleSaver = IsDlgButtonChecked(g_main, IDC_OPT_IDLE) == BST_CHECKED;
    s.performance = IsDlgButtonChecked(g_main, IDC_OPT_PERF) == BST_CHECKED;
    s.closeToTray = IsDlgButtonChecked(g_main, IDC_OPT_TRAY) == BST_CHECKED;
    s.gpuFallback = IsDlgButtonChecked(g_main, IDC_OPT_GPUFB) == BST_CHECKED;
    s.customPerfPlan = IsDlgButtonChecked(g_main, IDC_OPT_PERFPLAN) == BST_CHECKED;
    s.loadThreshold = std::clamp(_wtoi(GetWText(GetDlgItem(g_main, IDC_OPT_THRESH)).c_str()), 1, 100);
    s.procThreshold = std::clamp(_wtoi(GetWText(GetDlgItem(g_main, IDC_OPT_PROC)).c_str()), 1, 100);
    s.exclusions.clear();
    std::wstring text = GetWText(GetDlgItem(g_main, IDC_OPT_EXCL)) + L"\n";
    std::wstring cur;
    for (wchar_t ch : text) {
        if (ch == L'\r' || ch == L'\n' || ch == L',') {
            std::wstring t = StripText(cur);
            if (!t.empty()) s.exclusions.push_back(t);
            cur.clear();
        } else cur += ch;
    }
}
static void WriteOptionsToControls(bool withAutostart) {
    const Settings& s = g_sv.Cfg();
    CheckDlgButton(g_main, IDC_OPT_THROTTLE, s.throttle ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_main, IDC_OPT_BOOST, s.boost ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_main, IDC_OPT_AUTO, s.autoApply ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_main, IDC_OPT_FORCED, s.forced ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_main, IDC_OPT_SMART, s.memoryOptimizer ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_main, IDC_OPT_IDLE, s.idleSaver ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_main, IDC_OPT_PERF, s.performance ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_main, IDC_OPT_TRAY, s.closeToTray ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_main, IDC_OPT_GPUFB, s.gpuFallback ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_main, IDC_OPT_PERFPLAN, s.customPerfPlan ? BST_CHECKED : BST_UNCHECKED);
    if (withAutostart) {
        g_startupOn = ProcessSupervisor::GetAutostart();
        CheckDlgButton(g_main, IDC_OPT_AUTOSTART, g_startupOn ? BST_CHECKED : BST_UNCHECKED);
    }
    SetDlgItemInt(g_main, IDC_OPT_THRESH, s.loadThreshold, FALSE);
    SetDlgItemInt(g_main, IDC_OPT_PROC, s.procThreshold, FALSE);
    std::wstring ex;
    for (const auto& e : s.exclusions) ex += e + L"\r\n";
    SetWindowTextW(GetDlgItem(g_main, IDC_OPT_EXCL), ex.c_str());
}
static bool PersistNow();
// Changes settings from menus/buttons: controls are read first so nothing typed is lost.
static void ModifySettings(const std::function<void(Settings&)>& fn) {
    SyncRulesFromList();
    SyncOptionsFromControls();
    fn(g_sv.Cfg());
    WriteOptionsToControls(false);
    PersistNow();
}
static void ToggleSetting(bool Settings::*member) {
    ModifySettings([member](Settings& s) { s.*member = !(s.*member); });
}
static bool PersistNow() {
    SyncRulesFromList();
    SyncOptionsFromControls();
    return g_sv.SaveConfigTo(g_sv.ProfilePath(g_curProfile));
}

// ---------- process list ----------
using ViewRow = std::pair<int, int>;

static std::vector<ViewRow> BuildView(const std::wstring& filter, bool active) {
    std::vector<ViewRow> out;
    auto keep = [&](const ProcessInfo& p) {
        if (!filter.empty() && LowerW(p.name).find(filter) == std::wstring::npos) return false;
        if (active && !(p.cpuPercent >= 0.1 || g_sv.IsLowered(p.pid) || g_sv.IsBoosted(p.pid))) return false;
        return true;
    };
    bool tree = g_sv.Cfg().treeView && filter.empty() && !active;
    if (!tree) {
        for (int i = 0; i < (int)g_procs.size(); ++i)
            if (keep(g_procs[i])) out.push_back({ i, 0 });
        return out;
    }
    std::map<DWORD, int> pos;
    for (int i = 0; i < (int)g_procs.size(); ++i) pos[g_procs[i].pid] = i;
    std::map<int, std::vector<int>> kids;
    std::vector<int> roots;
    for (int i = 0; i < (int)g_procs.size(); ++i) {
        const ProcessInfo& p = g_procs[i];
        auto f = pos.find(p.parentPid);
        if (f != pos.end() && f->second != i && p.parentPid != p.pid) kids[f->second].push_back(i);
        else roots.push_back(i);
    }
    std::set<int> seen;
    std::function<void(int, int)> walk = [&](int i, int d) {
        if (seen.count(i)) return;
        seen.insert(i);
        out.push_back({ i, d });
        for (int c : kids[i]) walk(c, d + 1);
    };
    for (int r : roots) walk(r, 0);
    for (int i = 0; i < (int)g_procs.size(); ++i)
        if (!seen.count(i)) walk(i, 0);
    return out;
}

static int IconIndexFor(const std::wstring& path) {
    if (path.empty()) return -1;
    auto it = g_iconCache.find(path);
    if (it != g_iconCache.end()) return it->second;
    SHFILEINFOW sfi = {};
    int idx = -1;
    if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_SMALLICON) && sfi.hIcon) {
        idx = ImageList_AddIcon(g_imgList, sfi.hIcon);
        DestroyIcon(sfi.hIcon);
    }
    g_iconCache[path] = idx;
    return idx;
}

static void RebuildColumns() {
    while (ListView_DeleteColumn(g_proc, 0)) {}
    g_vis.clear();
    for (int c = 0; c < C_COUNT; ++c)
        if (c == C_NAME || ((g_sv.Cfg().columnsMask >> c) & 1)) g_vis.push_back(c);
    for (int j = 0; j < (int)g_vis.size(); ++j) {
        int c = g_vis[j];
        std::wstring t = kColTitle[c];
        if (c == C_CPU && !g_sv.Cfg().cpuUtility) t = L"CPU time";
        AddColumn(g_proc, j, t.c_str(), kColWidth[c]);
    }
}

// Keeps the scroll position of a list across a rebuild (DeleteAllItems resets it).
static void KeepScroll(HWND lv, const std::function<void()>& fill) {
    int top = ListView_GetTopIndex(lv);
    fill();
    int count = ListView_GetItemCount(lv);
    if (top <= 0 || count == 0) return;
    RECT r{};
    if (!ListView_GetItemRect(lv, 0, &r, LVIR_BOUNDS)) return;
    int rowH = r.bottom - r.top;
    if (rowH <= 0) return;
    int target = std::min(top, count - 1);
    ListView_Scroll(lv, 0, (target - ListView_GetTopIndex(lv)) * rowH);
}
static void FillProcListImpl();
static void FillProcList() { KeepScroll(g_proc, [] { FillProcListImpl(); }); }
static void FillProcListImpl() {
    ProcessInfo sel;
    DWORD selPid = CurrentSel(sel) ? sel.pid : 0;
    std::wstring filter = LowerW(StripText(GetWText(g_filter)));
    std::vector<ViewRow> view = BuildView(filter, g_activeOnly);
    ULONGLONG now = GetTickCount64();
    const Settings& cfg = g_sv.Cfg();

    SendMessageW(g_proc, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_proc);
    for (int row = 0; row < (int)view.size(); ++row) {
        int idx = view[row].first;
        const ProcessInfo& p = g_procs[idx];
        std::wstring name = std::wstring((size_t)view[row].second * 3, L' ') + p.name;
        LVITEMW it = {};
        it.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
        it.iItem = row;
        it.lParam = idx;
        it.pszText = const_cast<LPWSTR>(name.c_str());
        it.iImage = cfg.icons ? IconIndexFor(p.path) : I_IMAGENONE;
        ListView_InsertItem(g_proc, &it);

        bool lowered = g_sv.IsLowered(p.pid);
        wchar_t cpu[48];
        if (cfg.cpuUtility) swprintf_s(cpu, L"%.1f", p.cpuPercent);
        else swprintf_s(cpu, L"%llu:%02llu.%02llu", p.cpuTimeMs / 60000, (p.cpuTimeMs / 1000) % 60, (p.cpuTimeMs / 10) % 100);
        std::wstring pb;
        if (lowered) pb = std::to_wstring((now - g_sv.LoweredSince(p.pid)) / 1000) + L" s";

        std::wstring cells[C_COUNT];
        cells[C_USER] = p.user;
        cells[C_PID] = std::to_wstring(p.pid);
        cells[C_STATUS] = lowered ? L"Lowered" : g_sv.IsBoosted(p.pid) ? L"Boosted" : L"";
        cells[C_RULES] = g_sv.IsRuleControlled(p.name) ? L"X" : L"";
        cells[C_PRIO] = PriorityToLabel(p.priorityClass);
        cells[C_AFF] = AffToText(p.affinityMask);
        cells[C_CPU] = cpu;
        cells[C_THREADS] = std::to_wstring(p.threads);
        cells[C_PB] = pb;
        cells[C_MEM] = MemShortOf(p.memoryPriority);
        cells[C_ECO] = p.ecoQos ? L"On" : L"Off";
        cells[C_IO] = p.ioPriority <= 3 ? kIoLabels[p.ioPriority] : L"-";
        {
            wchar_t b[48];
            if (g_sv.GpuAvailable()) { swprintf_s(b, L"%.1f", p.gpuPercent); cells[C_GPU] = b; } else cells[C_GPU] = L"-";
            if (g_sv.GpuAvailable()) { swprintf_s(b, L"%.1f", p.gpuMemBytes / (1024.0 * 1024.0)); cells[C_GPUMEM] = b; } else cells[C_GPUMEM] = L"-";
            swprintf_s(b, L"%.1f", p.workingSetKB / 1024.0);
            cells[C_RAM] = b;
        }
        for (int j = 1; j < (int)g_vis.size(); ++j) SetSub(g_proc, row, j, cells[g_vis[j]]);

        if (selPid && p.pid == selPid) ListView_SetItemState(g_proc, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    SendMessageW(g_proc, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_proc, nullptr, FALSE);
}

static void SortProcs() {
    std::sort(g_procs.begin(), g_procs.end(), [](const ProcessInfo& a, const ProcessInfo& b) {
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
}

// ---------- action log ----------
static void RefreshLogImpl(bool force);
static void RefreshLog(bool force = false) { KeepScroll(g_log, [force] { RefreshLogImpl(force); }); }
static void RefreshLogImpl(bool force) {
    static ULONGLONG lastSeq = ~0ULL;
    static std::wstring lastFilter;
    std::wstring filter = LowerW(StripText(GetWText(GetDlgItem(g_main, IDC_LOGFILTER))));
    if (!force && lastSeq == g_sv.LogSeq() && lastFilter == filter) return;     // nothing new: keep the list
    lastSeq = g_sv.LogSeq();
    lastFilter = filter;
    SendMessageW(g_log, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_log);
    int row = 0;
    for (const LogEntry& e : g_sv.Log()) {
        if (!filter.empty() && LowerW(ProcessSupervisor::LogEntryText(e)).find(filter) == std::wstring::npos) continue;
        LVITEMW it = {};
        it.mask = LVIF_TEXT;
        it.iItem = row;
        it.pszText = const_cast<LPWSTR>(e.time.c_str());
        ListView_InsertItem(g_log, &it);
        SetSub(g_log, row, 1, e.computer);
        SetSub(g_log, row, 2, e.action);
        SetSub(g_log, row, 3, e.process);
        SetSub(g_log, row, 4, e.pid ? std::to_wstring(e.pid) : std::wstring());
        SetSub(g_log, row, 5, e.path);
        SetSub(g_log, row, 6, e.command);
        ++row;
    }
    SendMessageW(g_log, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_log, nullptr, FALSE);
}

// ---------- status bar ----------
static void UpdateStatus() {
    size_t threads = 0;
    for (const auto& p : g_procs) threads += p.threads;
    ULONGLONG up = GetTickCount64() / 1000;
    wchar_t upText[64];
    swprintf_s(upText, L"%llud %02llu:%02llu:%02llu", up / 86400, (up / 3600) % 24, (up / 60) % 60, up % 60);
    wchar_t ram[96];
    swprintf_s(ram, L"RAM: %d%% of %.0f GB", (int)(g_sv.MemoryLoad() + 0.5), g_sv.MemoryTotalGB());
    std::wstring gpuText = g_sv.GpuAvailable() ? L"GPU: " + std::to_wstring((int)(g_sv.GpuLoad() + 0.5)) + L"%"
                                               : std::wstring(L"GPU: n/a");
    std::wstring plan;
    std::wstring guid;
    if (g_sv.Cfg().showPlan && ProcessSupervisor::GetActivePowerPlan(guid)) plan = PlanName(guid);
    std::wstring text = L"CPU: " + std::to_wstring((int)(g_sv.SystemLoad() + 0.5)) + L"% time   |   " +
                        std::to_wstring(threads) + L" threads in " + std::to_wstring(g_procs.size()) +
                        L" processes   |   " + ram + L"   |   " + gpuText + L"   |   " + g_cpuName + L"   |   Uptime " + upText +
                        (plan.empty() ? L"" : L"   |   Plan: " + plan) +
                        (g_sv.EngineStopped() ? L"   |   Rules paused" : L"") +
                        L"   |   " + g_msg;
    text += L"   |   " + std::wstring(g_sv.ServiceRunning() ? L"Service: running" : L"Service: stopped") +
            L"   |   Disk " + std::to_wstring((int)(g_sv.DiskPercent() + 0.5)) + L"%   |   Net " +
            std::to_wstring((int)(g_sv.NetKBps() + 0.5)) + L" KB/s";
    text += L"   |   Smoothness " + std::to_wstring(SmoothnessIndex()) + L"%";
    SetWindowTextW(g_status, text.c_str());
}

// ---------- layout ----------
static void Place(int id, int x, int y, int w, int h) {
    MoveWindow(GetDlgItem(g_main, id), x, y, w, h, TRUE);
}
static void Layout(int W, int H) {
    int graphH = g_showGraph ? GRAPH_H : 0;
    int toolY = graphH + 4, toolH = 26;
    Place(IDC_VTAB, 4, toolY, 300, toolH);
    Place(IDC_FILTER, W - 214, toolY + 2, 210, 22);
    Place(IDC_HIDEGRAPH, W - 334, toolY, 110, toolH);
    Place(IDC_EDITRULES, W - 444, toolY, 100, toolH);
    Place(IDC_PAUSE, W - 504, toolY, 56, toolH);

    int listY = toolY + toolH + 6;
    int bottomY = H - STATUS_H - 6 - BOTTOM_H;
    Place(IDC_PROCLIST, 4, listY, W - 8, bottomY - 6 - listY);

    Place(IDC_BTAB, 4, bottomY, 340, 26);
    Place(IDC_SUMMARY, W - 420, bottomY, 100, 26);
    Place(IDC_VIEWLOG, W - 314, bottomY, 100, 26);
    Place(IDC_LOGFILTER, W - 208, bottomY + 2, 200, 22);
    int py = bottomY + 30;
    int pb = H - STATUS_H - 8;
    Place(IDC_LOG_CLEAR, 8, py, 110, 26);
    Place(IDC_LOG, 8, py + 30, W - 16, pb - py - 30);
    Place(IDC_THREADLIST, 8, py + 30, W - 16, pb - py - 30);
    Place(IDC_TH_HINT, 8, py + 4, W - 16, 22);
    Place(IDC_DEL_RULE, 8, py, 110, 26);
    Place(IDC_RUN_RULES, 124, py, 150, 26);
    Place(IDC_EXPORT, 280, py, 110, 26);
    Place(IDC_IMPORT, 396, py, 110, 26);
    Place(IDC_RULELIST, 8, py + 30, W - 16, pb - py - 30);

    // options: three columns
    int c1 = 14, c2 = 390, c3 = 660;
    int y = py + 4;
    Place(IDC_OPT_THROTTLE, c1, y, 360, 22); y += 26;
    Place(IDC_LBL_THRESH, c1 + 22, y + 4, 250, 20);
    Place(IDC_OPT_THRESH, c1 + 290, y, 60, 24); y += 28;
    Place(IDC_LBL_PROC, c1 + 22, y + 4, 250, 20);
    Place(IDC_OPT_PROC, c1 + 290, y, 60, 24); y += 28;
    Place(IDC_OPT_BOOST, c1, y, 360, 22); y += 26;
    Place(IDC_OPT_AUTO, c1, y, 360, 22); y += 30;
    Place(IDC_LBL_SVC, c1, y, 360, 20); y += 22;
    Place(IDC_SVC_INST, c1, y, 110, 24);
    Place(IDC_SVC_UNINST, c1 + 116, y, 110, 24);
    Place(IDC_SVC_START, c1 + 232, y, 60, 24);
    Place(IDC_SVC_STOP, c1 + 298, y, 60, 24);
    y = py + 4;
    Place(IDC_OPT_AUTOSTART, c2, y, 260, 22); y += 26;
    Place(IDC_OPT_FORCED, c2, y, 260, 22); y += 26;
    Place(IDC_OPT_SMART, c2, y, 260, 22); y += 26;
    Place(IDC_OPT_IDLE, c2, y, 260, 22); y += 26;
    Place(IDC_OPT_PERF, c2, y, 260, 22); y += 26;
    Place(IDC_OPT_TRAY, c2, y, 260, 22); y += 26;
    Place(IDC_OPT_GPUFB, c2, y, 260, 22); y += 26;
    Place(IDC_OPT_PERFPLAN, c2, y, 260, 22);
    Place(IDC_LBL_EXCL, c3, py + 4, W - c3 - 14, 20);
    Place(IDC_OPT_EXCL, c3, py + 26, W - c3 - 14, pb - py - 30);

    Place(IDC_STATUS, 6, H - STATUS_H - 2, W - 120, STATUS_H);
    Place(IDC_SAVE, W - 110, H - STATUS_H - 3, 104, STATUS_H + 2);
}
static void ShowBottom(int tab) {
    g_bottomTab = tab;
    for (int id : kLogIds) ShowWindow(GetDlgItem(g_main, id), tab == BOTTOM_LOG ? SW_SHOW : SW_HIDE);
    for (int id : kRuleIds) ShowWindow(GetDlgItem(g_main, id), tab == BOTTOM_RULES ? SW_SHOW : SW_HIDE);
    for (int id : kOptIds) ShowWindow(GetDlgItem(g_main, id), tab == BOTTOM_OPTIONS ? SW_SHOW : SW_HIDE);
    for (int id : kThreadIds) ShowWindow(GetDlgItem(g_main, id), tab == BOTTOM_THREADS ? SW_SHOW : SW_HIDE);
    TabCtrl_SetCurSel(g_btab, tab);
    if (tab == BOTTOM_LOG) RefreshLog(true);
    RECT rc;
    GetClientRect(g_main, &rc);
    Layout(rc.right, rc.bottom);
}

// ---------- graph (GDI+ into an off-screen bitmap, then one copy) ----------
static Gdiplus::Color ToGdi(COLORREF c, BYTE a = 255) {
    return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}
static void InvalidateGraph() {
    RECT rc;
    GetClientRect(g_main, &rc);
    RECT gr = { 0, 0, rc.right, g_showGraph ? GRAPH_H : 0 };
    InvalidateRect(g_main, &gr, FALSE);
}
static const COLORREF GC_CPU = RGB(70, 210, 70), GC_RAM = RGB(90, 150, 255), GC_GPU = RGB(255, 160, 40);

// One history line (values 0..100) over the last HIST_MAX samples; the newest sample is on the right.
static void DrawSeries(Gdiplus::Graphics& g, const std::vector<double>& h, COLORREF col, bool fill,
                       int gx, int gy, int gw, int gh, bool smooth) {
    int n = (int)h.size();
    if (n < 2) return;
    float step = (float)gw / (float)(HIST_MAX - 1);
    std::vector<Gdiplus::PointF> pts((size_t)n);
    for (int i = 0; i < n; ++i) {
        pts[i].X = gx + (HIST_MAX - n + i) * step;
        double v = std::clamp(h[i], 0.0, 100.0);
        pts[i].Y = gy + gh - 3 - (float)((gh - 6) * v / 100.0);
    }
    if (fill) {
        std::vector<Gdiplus::PointF> poly = pts;
        poly.push_back(Gdiplus::PointF(pts.back().X, (float)(gy + gh)));
        poly.push_back(Gdiplus::PointF(pts.front().X, (float)(gy + gh)));
        Gdiplus::SolidBrush fb(ToGdi(col, 40));
        g.FillPolygon(&fb, poly.data(), (INT)poly.size());
    }
    Gdiplus::Pen pen(ToGdi(col), 2.0f);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    if (smooth) g.DrawCurve(&pen, pts.data(), (INT)pts.size(), 0.5f);
    else g.DrawLines(&pen, pts.data(), (INT)pts.size());
}

static void PaintGraph(HDC dc, int W) {
    if (!g_showGraph) return;
    const Settings& s = g_sv.Cfg();
    int H = GRAPH_H;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, W, H);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    RECT all = { 0, 0, W, H };
    HBRUSH frame = CreateSolidBrush(RGB(60, 62, 62));
    FillRect(mem, &all, frame);
    DeleteObject(frame);

    int gx = 4, gy = 4, gw = W - 200, gh = H - 8;
    {
        Gdiplus::Graphics g(mem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::SolidBrush bg(ToGdi(RGB(38, 40, 40)));
        g.FillRectangle(&bg, gx, gy, gw, gh);

        int n = (int)g_hist.size();
        float step = (float)gw / (float)(HIST_MAX - 1);
        if (s.showEvents) {
            Gdiplus::SolidBrush ev(ToGdi(RGB(96, 96, 36), 200));
            for (int i = 0; i < n; ++i) {
                if (!g_evt[i]) continue;
                float x = gx + (HIST_MAX - n + i) * step;
                g.FillRectangle(&ev, x - step / 2, (float)gy, step * 1.6f, (float)gh);
            }
        }
        Gdiplus::Pen grid(ToGdi(RGB(255, 255, 255), 22), 1.0f);
        for (int q = 1; q < 4; ++q) {
            float y = gy + gh * q / 4.0f;
            g.DrawLine(&grid, (float)gx, y, (float)(gx + gw), y);
        }
        // RAM and GPU behind, CPU (filled) on top
        if (s.showRamLine) DrawSeries(g, g_histRam, GC_RAM, false, gx, gy, gw, gh, s.smooth);
        if (s.showGpuLine && g_sv.GpuAvailable()) DrawSeries(g, g_histGpu, GC_GPU, false, gx, gy, gw, gh, s.smooth);
        if (s.showCpuLine) DrawSeries(g, g_hist, GC_CPU, true, gx, gy, gw, gh, s.smooth);
    }

    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(245, 245, 245));
    if (s.showEvents) {
        HGDIOBJ of = SelectObject(mem, g_titleFont);
        std::wstring ev = std::to_wstring(g_sv.ThrottleTotal()) + L" auto-throttle events, " +
                          std::to_wstring(g_sv.ThrottleToday()) + L" today";
        RECT t1 = { gx + 8, gy + 4, gx + gw, gy + 26 };
        DrawTextW(mem, ev.c_str(), -1, &t1, DT_LEFT | DT_SINGLELINE);
        SelectObject(mem, of);
    }
    SelectObject(mem, g_smallFont);
    {   // legend with the current value of every line
        struct LegendItem { const wchar_t* name; COLORREF col; double val; bool on; bool avail; };
        LegendItem items[] = {
            { L"CPU", GC_CPU, g_sv.SystemLoad(), s.showCpuLine, true },
            { L"RAM", GC_RAM, g_sv.MemoryLoad(), s.showRamLine, true },
            { L"GPU", GC_GPU, g_sv.GpuLoad(), s.showGpuLine, g_sv.GpuAvailable() },
        };
        int lx = gx + 8, ly = gy + gh - 20;
        for (const LegendItem& it : items) {
            if (!it.on) continue;
            wchar_t t[48];
            if (it.avail) swprintf_s(t, L"%ls %d%%", it.name, (int)(it.val + 0.5));
            else swprintf_s(t, L"%ls n/a", it.name);
            RECT sq = { lx, ly + 4, lx + 9, ly + 13 };
            HBRUSH b = CreateSolidBrush(it.col);
            FillRect(mem, &sq, b);
            DeleteObject(b);
            SIZE sz = {};
            GetTextExtentPoint32W(mem, t, (int)wcslen(t), &sz);
            RECT tr = { lx + 14, ly, lx + 14 + sz.cx + 4, ly + 16 };
            SetTextColor(mem, RGB(245, 245, 245));
            DrawTextW(mem, t, -1, &tr, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
            lx += 14 + sz.cx + 18;
        }
    }
    if (s.showPlan) {
        std::wstring guid;
        std::wstring name = ProcessSupervisor::GetActivePowerPlan(guid) ? PlanName(guid) : L"";
        RECT t3 = { gx, gy + gh - 20, gx + gw - 8, gy + gh - 4 };
        DrawTextW(mem, name.c_str(), -1, &t3, DT_RIGHT | DT_SINGLELINE);
    }

    // meters: CPU, GPU, RAM
    int mx = W - 186, boxW = 56, gap = 8;
    const wchar_t* labels[] = { L"CPU", L"GPU", L"RAM" };
    for (int b = 0; b < 3; ++b) {
        if (b == 0 && !s.showCpuMeter) continue;
        if (b == 1 && !s.showGpuMeter) continue;
        if (b == 2 && !s.showRamMeter) continue;
        RECT box = { mx + b * (boxW + gap), 6, mx + b * (boxW + gap) + boxW, H - 6 };
        HBRUSH mbg = CreateSolidBrush(RGB(60, 60, 60));
        FillRect(mem, &box, mbg);
        DeleteObject(mbg);
        bool avail = (b != 1) || g_sv.GpuAvailable();
        double pct = (b == 0) ? g_sv.SystemLoad() : (b == 1) ? g_sv.GpuLoad() : g_sv.MemoryLoad();
        if (avail) {
            RECT fill = box;
            int hh = (int)((box.bottom - box.top) * std::clamp(pct, 0.0, 100.0) / 100.0);
            fill.top = box.bottom - hh;
            HBRUSH fb = CreateSolidBrush(b == 0 ? GC_CPU : b == 1 ? GC_GPU : GC_RAM);
            FillRect(mem, &fill, fb);
            DeleteObject(fb);
        }
        SetTextColor(mem, RGB(245, 245, 245));
        if (b == 0 && avail && !g_sv.CoreLoads().empty()) {
            const std::vector<double>& cores = g_sv.CoreLoads();
            int nc = (int)cores.size();
            int bw = std::max(1, static_cast<int>((box.right - box.left) / nc));
            for (int i = 0; i < nc; ++i) {
                int hc = (int)((box.bottom - box.top) * std::clamp(cores[i], 0.0, 100.0) / 100.0);
                RECT cb = { box.left + i * bw, box.bottom - hc, box.left + i * bw + bw - 1, box.bottom };
                HBRUSH cbr = CreateSolidBrush(RGB(150, 255, 150));
                FillRect(mem, &cb, cbr);
                DeleteObject(cbr);
            }
        }
        RECT lbl = { box.left, box.top + 2, box.right, box.top + 20 };
        DrawTextW(mem, labels[b], -1, &lbl, DT_CENTER | DT_SINGLELINE);
        wchar_t val[16];
        if (avail) swprintf_s(val, L"%d%%", (int)(pct + 0.5));
        else wcscpy_s(val, L"n/a");
        RECT vr = { box.left, box.bottom - 22, box.right, box.bottom - 4 };
        DrawTextW(mem, val, -1, &vr, DT_CENTER | DT_SINGLELINE);
    }
    BitBlt(dc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// ---------- timers ----------
static void SampleTick() {
    if (g_paused) return;
    g_sv.SampleSystemLoad();
    g_hist.push_back(g_sv.SystemLoad());
    g_histRam.push_back(g_sv.MemoryLoad());
    g_histGpu.push_back(g_sv.GpuLoad());
    g_evt.push_back(g_evtPending);
    g_evtPending = false;
    while ((int)g_hist.size() > HIST_MAX) {
        g_hist.erase(g_hist.begin());
        g_histRam.erase(g_histRam.begin());
        g_histGpu.erase(g_histGpu.begin());
        g_evt.erase(g_evt.begin());
    }
    InvalidateGraph();
}
static std::wstring ThreadPrioLabel(int v) {
    switch (v) {
    case -15: return L"Idle";
    case -2: return L"Lowest";
    case -1: return L"Below normal";
    case 0: return L"Normal";
    case 1: return L"Above normal";
    case 2: return L"Highest";
    case 15: return L"Time critical";
    case 0x7FFFFFFF: return L"Unknown";
    default: return L"Other";
    }
}
static void RefreshThreadsImpl();
static void RefreshThreads() { KeepScroll(g_thList, [] { RefreshThreadsImpl(); }); }
static void RefreshThreadsImpl() {
    if (!g_thList) return;
    ProcessInfo sel;
    if (!CurrentSel(sel)) {
        g_threads.clear();
        ListView_DeleteAllItems(g_thList);
        return;
    }
    DWORD keepTid = 0;
    int cur = ListView_GetNextItem(g_thList, -1, LVNI_SELECTED);
    if (cur >= 0 && cur < (int)g_threads.size()) keepTid = g_threads[cur].tid;
    g_threads = g_sv.EnumerateThreads(sel.pid);
    ListView_DeleteAllItems(g_thList);
    for (int i = 0; i < (int)g_threads.size(); ++i) {
        const ThreadInfo& t = g_threads[i];
        std::wstring tid = std::to_wstring(t.tid);
        LVITEMW it = {};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = i;
        it.lParam = i;
        it.pszText = const_cast<LPWSTR>(tid.c_str());
        ListView_InsertItem(g_thList, &it);
        wchar_t cpu[32];
        swprintf_s(cpu, L"%.1f", t.cpuPercent);
        SetSub(g_thList, i, 1, ThreadPrioLabel(t.priority));
        SetSub(g_thList, i, 2, cpu);
        SetSub(g_thList, i, 3, std::to_wstring(t.cpuTimeMs / 1000) + L" s");
        if (keepTid && t.tid == keepTid)
            ListView_SetItemState(g_thList, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
}
static void HandleThreadMenu(int cmd) {
    int sel = ListView_GetNextItem(g_thList, -1, LVNI_SELECTED);
    if (sel < 0) { SetStatus(L"Select a thread first."); return; }
    LVITEMW li = {};
    li.mask = LVIF_PARAM;
    li.iItem = sel;
    ListView_GetItem(g_thList, &li);
    if (li.lParam < 0 || li.lParam >= (LPARAM)g_threads.size()) return;
    DWORD tid = g_threads[(size_t)li.lParam].tid;
    bool ok = false;
    if (cmd >= IDM_TH_PRIO && cmd < IDM_TH_PRIO + 7) ok = g_sv.SetThreadPriorityLevel(tid, kThPrio[cmd - IDM_TH_PRIO]);
    else if (cmd >= IDM_TH_AFF && cmd <= IDM_TH_AFF + 2) ok = g_sv.SetThreadAffinityOf(tid, AffinityPreset(cmd - IDM_TH_AFF));
    else if (cmd == IDM_TH_BOOST_ON || cmd == IDM_TH_BOOST_OFF) ok = g_sv.SetThreadBoostOf(tid, cmd == IDM_TH_BOOST_ON);
    else if (cmd == IDM_TH_SUSPEND || cmd == IDM_TH_RESUME) ok = g_sv.SetThreadSuspendedOf(tid, cmd == IDM_TH_SUSPEND);
    SetStatus(ok ? L"Thread changed." : L"Thread change failed (missing permissions or thread ended).");
    RefreshThreads();
}
static void ShowThreadMenu() {
    static const wchar_t* names[7] = { L"Idle", L"Lowest", L"Below normal", L"Normal", L"Above normal", L"Highest", L"Time critical" };
    HMENU m = CreatePopupMenu(), pr = CreatePopupMenu(), af = CreatePopupMenu();
    for (int i = 0; i < 7; ++i) AppendMenuW(pr, MF_STRING, IDM_TH_PRIO + i, names[i]);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)pr, L"Thread Priority");
    AppendMenuW(af, MF_STRING, IDM_TH_AFF + 0, L"All CPUs");
    AppendMenuW(af, MF_STRING, IDM_TH_AFF + 1, L"First half of CPUs");
    AppendMenuW(af, MF_STRING, IDM_TH_AFF + 2, L"Second half of CPUs");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)af, L"Thread Affinity");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_TH_BOOST_ON, L"Priority boost: on");
    AppendMenuW(m, MF_STRING, IDM_TH_BOOST_OFF, L"Priority boost: off");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_TH_SUSPEND, L"Suspend thread");
    AppendMenuW(m, MF_STRING, IDM_TH_RESUME, L"Resume thread");
    POINT pt;
    GetCursorPos(&pt);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_main, nullptr);
    DestroyMenu(m);
    if (cmd) HandleThreadMenu(cmd);
}

static void ShowBalloon(const std::wstring& text) {
    if (!g_trayAdded) return;
    g_nid.uFlags = NIF_INFO | NIF_ICON | NIF_MESSAGE | NIF_TIP;
    wcscpy_s(g_nid.szInfoTitle, L"OpenLasso");
    wcsncpy_s(g_nid.szInfo, text.c_str(), _TRUNCATE);
    g_nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}
static void ProcsTick() {
    if (g_paused) return;
    g_sv.SampleGpu();
    g_procs = g_sv.EnumerateProcesses();
    SortProcs();
    bool viaService = g_sv.Cfg().delegateToService && g_sv.ServiceRunning();
    if (!viaService && !g_sv.EngineStopped() && g_sv.Cfg().autoApply) {
        SyncRulesFromList();
        SyncOptionsFromControls();
        DWORD before = g_sv.ThrottleTotal();
        g_sv.ApplyAll(g_procs);
        if (g_sv.ThrottleTotal() != before) g_evtPending = true;
    }
    for (const auto& note : g_sv.TakeNotes()) ShowBalloon(note);
    g_sv.TickKeepAwake();
    g_plans.clear();
    FillProcList();
    if (g_bottomTab == BOTTOM_LOG) RefreshLog();
    if (g_bottomTab == BOTTOM_THREADS) RefreshThreads();
    UpdateStatus();
}

// ---------- tray ----------
static void TrayUpdate() {
    const Settings& s = g_sv.Cfg();
    bool hidden = !IsWindowVisible(g_main) || IsIconic(g_main);
    bool want = (s.trayMode == 0) || (s.trayMode == 1 && hidden) || hidden;
    if (want && !g_trayAdded) {
        ZeroMemory(&g_nid, sizeof(g_nid));
        g_nid.cbSize = sizeof(g_nid);
        g_nid.hWnd = g_main;
        g_nid.uID = 1;
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_nid.uCallbackMessage = WM_TRAY;
        g_nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(2));
        if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(g_nid.szTip, L"OpenLasso");
        Shell_NotifyIconW(NIM_ADD, &g_nid);
        g_trayAdded = true;
    } else if (!want && g_trayAdded) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayAdded = false;
    }
}
static void ShowMain() {
    ShowWindow(g_main, SW_SHOW);
    ShowWindow(g_main, SW_RESTORE);
    SetForegroundWindow(g_main);
    TrayUpdate();
}
static void ShowTrayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_TRAY_SHOW, L"Show OpenLasso");
    AppendMenuW(m, MF_STRING, IDM_TRAY_PAUSE, g_paused ? L"Resume" : L"Pause");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_MAIN_EXIT, L"Exit OpenLasso");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_main);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_main, nullptr);
    DestroyMenu(m);
    if (cmd) SendMessageW(g_main, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
}

// ---------- process menu ----------
static HMENU BuildProcMenu() {
    HMENU m = CreatePopupMenu();
    auto sub = [](HMENU parent, HMENU child, const wchar_t* text) { AppendMenuW(parent, MF_POPUP, (UINT_PTR)child, text); };
    auto twoLists = [&](HMENU root, const wchar_t* title, UINT baseCur, UINT baseAlw, const wchar_t** names, int count, int firstValue) {
        HMENU h = CreatePopupMenu(), cur = CreatePopupMenu(), alw = CreatePopupMenu();
        for (int i = 0; i < count; ++i) {
            AppendMenuW(cur, MF_STRING, baseCur + i, names[i]);
            AppendMenuW(alw, MF_STRING, baseAlw + i, names[i]);
        }
        (void)firstValue;
        sub(h, cur, L"Current");
        sub(h, alw, L"Always");
        sub(root, h, title);
    };
    static std::vector<const wchar_t*> prioNames;
    prioNames.clear();
    for (int i = 0; i < kPriorityCount; ++i) prioNames.push_back(kPriorities[i].label);
    twoLists(m, L"CPU Priority", IDM_CTX_PRIO, IDM_CTX_PRIO_ALW, prioNames.data(), kPriorityCount, 0);
    const wchar_t* affNames[] = { L"All CPUs", L"First half of CPUs", L"Second half of CPUs" };
    twoLists(m, L"CPU Affinity", IDM_CTX_AFF, IDM_CTX_AFF_ALW, affNames, 3, 0);
    const wchar_t* ioNames[] = { L"Very low", L"Low", L"Normal", L"High" };
    twoLists(m, L"I/O Priority", IDM_CTX_IO, IDM_CTX_IO_ALW, ioNames, 4, 0);
    {
        const wchar_t* gpuNames[] = { L"Idle (low)", L"Below normal", L"Normal", L"Above normal", L"High", L"Realtime (risky)" };
        twoLists(m, L"GPU Priority", IDM_CTX_GPUPRIO + 1, IDM_CTX_GPUPRIO_ALW + 1, gpuNames, 6, 0);
        const wchar_t* prefNames[] = { L"Default", L"Power saving", L"High performance" };
        twoLists(m, L"GPU Preference", IDM_CTX_GPUPREF, IDM_CTX_GPUPREF_ALW, prefNames, 3, 0);
    }
    const wchar_t* memNames[] = { L"Very low", L"Low", L"Medium", L"Below normal", L"Normal" };
    twoLists(m, L"Memory Priority", IDM_CTX_MEM + 1, IDM_CTX_MEM_ALW + 1, memNames, 5, 1);
    HMENU eco = CreatePopupMenu(), ecoCur = CreatePopupMenu(), ecoAlw = CreatePopupMenu();
    AppendMenuW(ecoCur, MF_STRING, IDM_CTX_ECO_ON, L"On");
    AppendMenuW(ecoCur, MF_STRING, IDM_CTX_ECO_OFF, L"Off");
    AppendMenuW(ecoAlw, MF_STRING, IDM_CTX_ECO_ON_ALW, L"On");
    AppendMenuW(ecoAlw, MF_STRING, IDM_CTX_ECO_OFF_ALW, L"Off");
    sub(eco, ecoCur, L"Current");
    sub(eco, ecoAlw, L"Always");
    sub(m, eco, L"Efficiency Mode");
    {
        const wchar_t* setNames[] = { L"All CPUs", L"First half of CPUs", L"Second half of CPUs" };
        twoLists(m, L"CPU Sets", IDM_CTX_CPUSET + 1, IDM_CTX_CPUSET_ALW + 1, setNames, 3, 0);
    }
    HMENU tb = CreatePopupMenu(), tbCur = CreatePopupMenu(), tbAlw = CreatePopupMenu();
    AppendMenuW(tbCur, MF_STRING, IDM_CTX_TB_ON, L"On");
    AppendMenuW(tbCur, MF_STRING, IDM_CTX_TB_OFF, L"Off");
    AppendMenuW(tbAlw, MF_STRING, IDM_CTX_TB_ON_ALW, L"On");
    AppendMenuW(tbAlw, MF_STRING, IDM_CTX_TB_OFF_ALW, L"Off");
    sub(tb, tbCur, L"Current");
    sub(tb, tbAlw, L"Always");
    sub(m, tb, L"Thread Boost");
    {
        HMENU pf = CreatePopupMenu(), pfc = CreatePopupMenu(), pfa = CreatePopupMenu();
        AppendMenuW(pfc, MF_STRING, IDM_CTX_PERF_ON, L"On");
        AppendMenuW(pfc, MF_STRING, IDM_CTX_PERF_OFF, L"Off");
        AppendMenuW(pfa, MF_STRING, IDM_CTX_PERF_ON_ALW, L"On");
        AppendMenuW(pfa, MF_STRING, IDM_CTX_PERF_OFF_ALW, L"Off");
        sub(pf, pfc, L"Current");
        sub(pf, pfa, L"Always");
        sub(m, pf, L"Force High-Power Plan");
    }
    HMENU plan = CreatePopupMenu();
    AppendMenuW(plan, MF_STRING, IDM_CTX_PLAN_NONE, L"None (remove rule)");
    g_plans = ProcessSupervisor::ListPowerPlans();
    for (int i = 0; i < (int)g_plans.size(); ++i) AppendMenuW(plan, MF_STRING, IDM_CTX_PLAN + i, g_plans[i].name.c_str());
    sub(m, plan, L"Power Profile");
    {
        HMENU sl = CreatePopupMenu(), slc = CreatePopupMenu(), sla = CreatePopupMenu();
        AppendMenuW(slc, MF_STRING, IDM_CTX_SLEEP_ON, L"On");
        AppendMenuW(slc, MF_STRING, IDM_CTX_SLEEP_OFF, L"Off");
        AppendMenuW(sla, MF_STRING, IDM_CTX_SLEEP_ON_ALW, L"On");
        AppendMenuW(sla, MF_STRING, IDM_CTX_SLEEP_OFF_ALW, L"Off");
        sub(sl, slc, L"Current");
        sub(sl, sla, L"Always");
        sub(m, sl, L"Block Sleep");
    }
    {
        HMENU kr = CreatePopupMenu();
        AppendMenuW(kr, MF_STRING, IDM_CTX_KEEP_ON, L"Keep running (restart with original arguments)");
        AppendMenuW(kr, MF_STRING, IDM_CTX_KEEP_ON_ELEV, L"Keep running (restart as administrator)");
        AppendMenuW(kr, MF_STRING, IDM_CTX_KEEP_CMD, L"Set restart arguments...");
        AppendMenuW(kr, MF_STRING, IDM_CTX_KEEP_OFF, L"Stop keeping it running");
        sub(m, kr, L"Keep Running");
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_CTX_ALLGROUPS, L"Use all processor groups (64+ CPUs)");
    {
        HMENU mt = CreatePopupMenu();
        AppendMenuW(mt, MF_STRING, IDM_CTX_MATCH_NAME, L"Match by program name");
        AppendMenuW(mt, MF_STRING, IDM_CTX_MATCH_FOLDER, L"Match by folder");
        AppendMenuW(mt, MF_STRING, IDM_CTX_MATCH_PATH, L"Match by full path");
        AppendMenuW(mt, MF_STRING, IDM_CTX_MATCH_DEFAULT, L"Default rule (all other programs)");
        sub(m, mt, L"Rule match");
        HMENU lim = CreatePopupMenu();
        AppendMenuW(lim, MF_STRING, IDM_CTX_LIMIT25, L"Cap CPU to 25% (always)");
        AppendMenuW(lim, MF_STRING, IDM_CTX_LIMIT50, L"Cap CPU to 50% (always)");
        AppendMenuW(lim, MF_STRING, IDM_CTX_LIMIT_OFF, L"Remove CPU limit");
        sub(m, lim, L"CPU Cap");
        HMENU wd = CreatePopupMenu();
        AppendMenuW(wd, MF_STRING, IDM_CTX_WD_LOW, L"Lower priority after 10 s above 50% CPU");
        AppendMenuW(wd, MF_STRING, IDM_CTX_WD_KILL, L"Close after 60 s above 90% CPU");
        AppendMenuW(wd, MF_STRING, IDM_CTX_WD_OFF, L"Remove CPU guard");
        sub(m, wd, L"CPU guard");
        AppendMenuW(m, MF_STRING, IDM_CTX_BLOCK, L"Block (close when it starts)");
        AppendMenuW(m, MF_STRING, IDM_CTX_BALANCE, L"Balance instances across CPUs");
    }
    AppendMenuW(m, MF_STRING, IDM_CTX_EXCL, L"Exclude from auto-throttle");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    HMENU more = CreatePopupMenu();
    AppendMenuW(more, MF_STRING, IDM_CTX_COPY_RULES, L"Copy rules");
    AppendMenuW(more, MF_STRING, IDM_CTX_PASTE_RULES, L"Paste rules");
    AppendMenuW(more, MF_STRING, IDM_CTX_REMOVE_RULES, L"Remove all rules for this process");
    sub(m, more, L"More");
    AppendMenuW(m, MF_STRING, IDM_CTX_FREEMEM, L"Trim Memory");
    HMENU loc = CreatePopupMenu();
    AppendMenuW(loc, MF_STRING, IDM_CTX_LOCATE, L"Open file location");
    sub(m, loc, L"Locate");
    HMENU copy = CreatePopupMenu();
    AppendMenuW(copy, MF_STRING, IDM_CTX_COPY_NAME, L"Name");
    AppendMenuW(copy, MF_STRING, IDM_CTX_COPY_PATH, L"Path");
    AppendMenuW(copy, MF_STRING, IDM_CTX_COPY_PID, L"PID");
    sub(m, copy, L"Copy");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_CTX_RESTART, L"Restart");
    AppendMenuW(m, MF_STRING, IDM_CTX_RESTART_ADMIN, L"Restart as Admin");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_CTX_CLOSE, L"Close");
    AppendMenuW(m, MF_STRING, IDM_CTX_TERM, L"Terminate");
    return m;
}

static void ApplyNow(const ProcessInfo& p, const ProcessRule& r) {
    if (r.priorityClass == REALTIME_PRIORITY_CLASS &&
        MessageBoxW(g_main, L"Realtime priority can make the system unresponsive. Continue?",
                    L"Warning", MB_YESNO | MB_ICONWARNING) != IDYES) return;
    bool ok = g_sv.ApplyToProcess(p.pid, r, p.path, &p);
    g_sv.AddLog(L"Manual change applied to " + p.name + (ok ? L"" : L" (failed)"), &p);
    const std::wstring& note = g_sv.LastNote();       // e.g. "the GPU driver ignored the request"
    SetStatus(!note.empty() ? note : std::wstring(ok ? L"Change applied." : L"Failed: missing permissions or protected process."));
    ProcsTick();
}

static void HandleProcMenu(int cmd) {
    ProcessInfo p;
    if (!CurrentSel(p)) { SetStatus(L"Select a process first."); return; }
    p.cmdLine = g_sv.CommandLineOf(p.pid);            // read now: it is gone once the process ends
    auto alwaysRule = [&](const std::function<void(ProcessRule&)>& fn) {
        SetRuleField(p.name, fn);
        PersistNow();
    };
    if (cmd >= IDM_CTX_PRIO && cmd < IDM_CTX_PRIO + kPriorityCount) {
        ProcessRule r; r.priorityClass = kPriorities[cmd - IDM_CTX_PRIO].value;
        ApplyNow(p, r);
    } else if (cmd >= IDM_CTX_PRIO_ALW && cmd < IDM_CTX_PRIO_ALW + kPriorityCount) {
        DWORD v = kPriorities[cmd - IDM_CTX_PRIO_ALW].value;
        ProcessRule r; r.priorityClass = v;
        ApplyNow(p, r);
        alwaysRule([v](ProcessRule& x) { x.priorityClass = v; });
    } else if (cmd >= IDM_CTX_AFF && cmd <= IDM_CTX_AFF + 2) {
        ProcessRule r; r.affinityMask = AffinityPreset(cmd - IDM_CTX_AFF);
        ApplyNow(p, r);
    } else if (cmd >= IDM_CTX_AFF_ALW && cmd <= IDM_CTX_AFF_ALW + 2) {
        DWORD_PTR m = AffinityPreset(cmd - IDM_CTX_AFF_ALW);
        ProcessRule r; r.affinityMask = m;
        ApplyNow(p, r);
        alwaysRule([m](ProcessRule& x) { x.affinityMask = m; });
    } else if (cmd >= IDM_CTX_IO && cmd <= IDM_CTX_IO + 3) {
        ProcessRule r; r.ioPriority = cmd - IDM_CTX_IO + 1;
        ApplyNow(p, r);
    } else if (cmd >= IDM_CTX_IO_ALW && cmd <= IDM_CTX_IO_ALW + 3) {
        DWORD v = cmd - IDM_CTX_IO_ALW + 1;
        ProcessRule r; r.ioPriority = v;
        ApplyNow(p, r);
        alwaysRule([v](ProcessRule& x) { x.ioPriority = v; });
    } else if (cmd >= IDM_CTX_MEM + 1 && cmd <= IDM_CTX_MEM + 5) {
        ProcessRule r; r.memoryPriority = cmd - IDM_CTX_MEM;
        ApplyNow(p, r);
    } else if (cmd >= IDM_CTX_MEM_ALW + 1 && cmd <= IDM_CTX_MEM_ALW + 5) {
        DWORD v = cmd - IDM_CTX_MEM_ALW;
        ProcessRule r; r.memoryPriority = v;
        ApplyNow(p, r);
        alwaysRule([v](ProcessRule& x) { x.memoryPriority = v; });
    } else if (cmd == IDM_CTX_ECO_ON || cmd == IDM_CTX_ECO_OFF) {
        ProcessRule r; r.eco = (cmd == IDM_CTX_ECO_ON) ? 1 : 2;
        ApplyNow(p, r);
    } else if (cmd == IDM_CTX_ECO_ON_ALW || cmd == IDM_CTX_ECO_OFF_ALW) {
        int e = (cmd == IDM_CTX_ECO_ON_ALW) ? 1 : 2;
        ProcessRule r; r.eco = e;
        ApplyNow(p, r);
        alwaysRule([e](ProcessRule& x) { x.eco = e; });
    } else if (cmd == IDM_CTX_TB_ON || cmd == IDM_CTX_TB_OFF) {
        ProcessRule r; r.threadBoost = (cmd == IDM_CTX_TB_ON) ? 1 : 2;
        ApplyNow(p, r);
    } else if (cmd == IDM_CTX_TB_ON_ALW || cmd == IDM_CTX_TB_OFF_ALW) {
        int t = (cmd == IDM_CTX_TB_ON_ALW) ? 1 : 2;
        ProcessRule r; r.threadBoost = t;
        ApplyNow(p, r);
        alwaysRule([t](ProcessRule& x) { x.threadBoost = t; });
    } else if (cmd == IDM_CTX_PLAN_NONE) {
        alwaysRule([](ProcessRule& x) { x.powerPlan.clear(); });
        SetStatus(L"Power profile rule removed for " + p.name);
    } else if (cmd >= IDM_CTX_PLAN && cmd < IDM_CTX_PLAN + 100) {
        int i = cmd - IDM_CTX_PLAN;
        if (i < (int)g_plans.size()) {
            std::wstring guid = g_plans[i].guid;
            alwaysRule([guid](ProcessRule& x) { x.powerPlan = guid; });
            SetStatus(L"Power profile set for " + p.name);
        }
    } else if (cmd >= IDM_CTX_CPUSET + 1 && cmd <= IDM_CTX_CPUSET + 3) {
        ProcessRule r; r.cpuSets = cmd - IDM_CTX_CPUSET;
        ApplyNow(p, r);
    } else if (cmd >= IDM_CTX_CPUSET_ALW + 1 && cmd <= IDM_CTX_CPUSET_ALW + 3) {
        int m = cmd - IDM_CTX_CPUSET_ALW;
        ProcessRule r; r.cpuSets = m;
        ApplyNow(p, r);
        alwaysRule([m](ProcessRule& x) { x.cpuSets = m; });
    } else if (cmd >= IDM_CTX_GPUPRIO + 1 && cmd <= IDM_CTX_GPUPRIO + 6) {
        ProcessRule r; r.gpuPriority = cmd - IDM_CTX_GPUPRIO;
        ApplyNow(p, r);
    } else if (cmd >= IDM_CTX_GPUPRIO_ALW + 1 && cmd <= IDM_CTX_GPUPRIO_ALW + 6) {
        int g = cmd - IDM_CTX_GPUPRIO_ALW;
        ProcessRule r; r.gpuPriority = g;
        ApplyNow(p, r);
        alwaysRule([g](ProcessRule& x) { x.gpuPriority = g; });
    } else if (cmd >= IDM_CTX_GPUPREF && cmd <= IDM_CTX_GPUPREF + 2) {
        ProcessRule r; r.gpuPref = cmd - IDM_CTX_GPUPREF + 1;
        ApplyNow(p, r);
    } else if (cmd >= IDM_CTX_GPUPREF_ALW && cmd <= IDM_CTX_GPUPREF_ALW + 2) {
        int g = cmd - IDM_CTX_GPUPREF_ALW + 1;
        ProcessRule r; r.gpuPref = g;
        ApplyNow(p, r);
        alwaysRule([g](ProcessRule& x) { x.gpuPref = g; });
    } else if (cmd == IDM_CTX_SLEEP_ON || cmd == IDM_CTX_SLEEP_OFF) {
        bool on = cmd == IDM_CTX_SLEEP_ON;
        g_sv.SetSleepHold(p.pid, on);
        SetStatus(on ? L"Block sleep while this process runs." : L"Block sleep turned off.");
    } else if (cmd == IDM_CTX_SLEEP_ON_ALW || cmd == IDM_CTX_SLEEP_OFF_ALW) {
        bool on = cmd == IDM_CTX_SLEEP_ON_ALW;
        g_sv.SetSleepHold(p.pid, on);
        alwaysRule([on](ProcessRule& x) { x.preventSleep = on; });
        SetStatus(on ? L"Block sleep saved as rule." : L"Block sleep rule removed.");
    } else if (cmd == IDM_CTX_PERF_ON || cmd == IDM_CTX_PERF_OFF) {
        bool on = cmd == IDM_CTX_PERF_ON;
        g_sv.SetPerfHold(p.pid, on);
        SetStatus(on ? L"High-power mode while this process runs." : L"High-power mode turned off.");
    } else if (cmd == IDM_CTX_PERF_ON_ALW || cmd == IDM_CTX_PERF_OFF_ALW) {
        bool on = cmd == IDM_CTX_PERF_ON_ALW;
        g_sv.SetPerfHold(p.pid, on);
        alwaysRule([on](ProcessRule& x) { x.forcePower = on; });
        SetStatus(on ? L"High-power mode saved as rule." : L"High-power mode rule removed.");
    } else if (cmd == IDM_CTX_KEEP_ON || cmd == IDM_CTX_KEEP_ON_ELEV) {
        bool elev = (cmd == IDM_CTX_KEEP_ON_ELEV);
        std::wstring args = ProcessSupervisor::StripArgv0(p.cmdLine);
        std::wstring path = p.path;
        if (path.empty()) { SetStatus(L"The file path is not available, so this process cannot be restarted."); return; }
        alwaysRule([=](ProcessRule& x) {
            x.keepRunning = true;
            x.keepRunningElevate = elev;
            x.keepRunningPath = path;
            x.keepRunningCommandLine = args;
        });
        g_sv.AddLog(L"Keep running enabled" + std::wstring(elev ? L" (as administrator)" : L""), &p);
        SetStatus(p.name + L" will be restarted with its original arguments if it exits.");
    } else if (cmd == IDM_CTX_KEEP_CMD) {
        std::wstring cur;
        for (const auto& r : g_sv.Rules())
            if (_wcsicmp(r.processName.c_str(), p.name.c_str()) == 0 && r.keepRunning) cur = r.keepRunningCommandLine;
        if (cur.empty()) cur = ProcessSupervisor::StripArgv0(p.cmdLine);
        if (!InputBox(L"Restart arguments", L"Arguments used when the process is restarted:", cur)) return;
        std::wstring path = p.path;
        alwaysRule([=](ProcessRule& x) {
            x.keepRunning = true;
            x.keepRunningCommandLine = cur;
            if (x.keepRunningPath.empty()) x.keepRunningPath = path;
        });
        SetStatus(L"Restart arguments saved for " + p.name);
    } else if (cmd == IDM_CTX_KEEP_OFF) {
        alwaysRule([](ProcessRule& x) { x.keepRunning = false; });
        g_sv.AddLog(L"Keep running disabled", &p);
        SetStatus(L"Keep running disabled for " + p.name);
    } else if (cmd == IDM_CTX_RESTART_ADMIN) {
        bool ok = g_sv.RestartProcess(p, true);
        g_sv.AddLog(L"Restarted as administrator with its original arguments" + std::wstring(ok ? L"" : L" (failed)"), &p);
        SetStatus(ok ? L"Process restarted as administrator, with its original arguments." : L"Could not restart the process.");
        ProcsTick();
    } else if (cmd == IDM_CTX_ALLGROUPS) {
        bool ok = g_sv.SetAffinityAllGroups(p.pid);
        g_sv.AddLog(L"All processor groups for " + p.name + (ok ? L"" : L" (failed)"), &p);
        SetStatus(ok ? L"Process may use all " + std::to_wstring(g_sv.LogicalProcessorCount()) + L" logical CPUs."
                     : L"Could not change processor groups for this process.");
    } else if (cmd == IDM_CTX_MATCH_PATH || cmd == IDM_CTX_MATCH_FOLDER || cmd == IDM_CTX_MATCH_NAME || cmd == IDM_CTX_MATCH_DEFAULT) {
        std::wstring pat = cmd == IDM_CTX_MATCH_PATH ? p.path
            : cmd == IDM_CTX_MATCH_FOLDER ? p.path.substr(0, p.path.find_last_of(L"\\/") + 1)
            : cmd == IDM_CTX_MATCH_DEFAULT ? std::wstring(L"*") : p.name;
        if (pat.empty()) { SetStatus(L"The process path is not available."); return; }
        SetRuleField(p.name, [pat](ProcessRule& x) { x.processName = pat; });
        PersistNow();
        SetStatus(L"Rule now matches: " + pat);
    } else if (cmd == IDM_CTX_LIMIT25 || cmd == IDM_CTX_LIMIT50 || cmd == IDM_CTX_LIMIT_OFF) {
        int v = cmd == IDM_CTX_LIMIT25 ? 25 : cmd == IDM_CTX_LIMIT50 ? 50 : 0;
        alwaysRule([v](ProcessRule& x) { x.limitPercent = v; });
        SetStatus(v ? L"CPU limit " + std::to_wstring(v) + L"% saved as rule." : L"CPU limit removed.");
    } else if (cmd == IDM_CTX_WD_LOW) {
        alwaysRule([](ProcessRule& x) { x.guardCpu = 50; x.guardSeconds = 10; x.guardAction = 1; });
        SetStatus(L"CPU guard saved: lower priority after 10 s above 50% CPU.");
    } else if (cmd == IDM_CTX_WD_KILL) {
        alwaysRule([](ProcessRule& x) { x.guardCpu = 90; x.guardSeconds = 60; x.guardAction = 2; });
        SetStatus(L"CPU guard saved: close after 60 s above 90% CPU.");
    } else if (cmd == IDM_CTX_WD_OFF) {
        alwaysRule([](ProcessRule& x) { x.guardCpu = 0; x.guardSeconds = 0; x.guardAction = 0; });
        SetStatus(L"CPU guard removed.");
    } else if (cmd == IDM_CTX_BLOCK || cmd == IDM_CTX_BALANCE) {
        bool cur = false;
        for (const auto& r : g_sv.Rules())
            if (_wcsicmp(r.processName.c_str(), p.name.c_str()) == 0) cur = (cmd == IDM_CTX_BLOCK) ? r.block : r.balanceInstances;
        bool nv = !cur;
        bool block = cmd == IDM_CTX_BLOCK;
        alwaysRule([nv, block](ProcessRule& x) { if (block) x.block = nv; else x.balanceInstances = nv; });
        SetStatus(std::wstring(block ? L"Auto-close " : L"Instance spreading ") + (nv ? L"on" : L"off") + L" for " + p.name);
    } else if (cmd == IDM_CTX_EXCL) {
        ModifySettings([&](Settings& s) {
            auto it = std::find_if(s.exclusions.begin(), s.exclusions.end(),
                [&](const std::wstring& e) { return _wcsicmp(e.c_str(), p.name.c_str()) == 0; });
            if (it != s.exclusions.end()) s.exclusions.erase(it);
            else s.exclusions.push_back(p.name);
        });
        SetStatus(p.name + L": Auto-throttle exclusion changed.");
    } else if (cmd == IDM_CTX_FREEMEM) {
        bool ok = g_sv.FreeProcessMemory(p.pid);
        SetStatus(ok ? L"Memory trimmed." : L"Could not trim memory.");
    } else if (cmd == IDM_CTX_CLOSE) {
        g_sv.NoteUserExit(p.name);
        SetStatus(g_sv.CloseProcessWindows(p.pid) ? L"Close requested." : L"This process has no window to close.");
    } else if (cmd == IDM_CTX_TERM) {
        if (MessageBoxW(g_main, (L"End process " + p.name + L"? Unsaved data may be lost.").c_str(),
                        L"End process", MB_YESNO | MB_ICONWARNING) != IDYES) return;
        g_sv.NoteUserExit(p.name);                    // an exit you ask for is not "restarted"
        bool ok = g_sv.EndProcess(p.pid);
        g_sv.AddLog(L"Process ended: " + p.name + (ok ? L"" : L" (failed)"), &p);
        SetStatus(ok ? L"Process ended." : L"Could not end the process.");
        ProcsTick();
    } else if (cmd == IDM_CTX_RESTART) {
        bool ok = g_sv.RestartProcess(p);
        g_sv.AddLog(L"Restarted with its original arguments" + std::wstring(ok ? L"" : L" (failed)"), &p);
        SetStatus(ok ? L"Process restarted with its original command-line arguments." : L"Could not restart the process.");
        ProcsTick();
    } else if (cmd == IDM_CTX_LOCATE) {
        if (p.path.empty()) { SetStatus(L"The file path is not available."); return; }
        std::wstring args = L"/select,\"" + p.path + L"\"";
        ShellExecuteW(g_main, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    } else if (cmd == IDM_CTX_COPY_NAME) {
        SetClipboardTextW(p.name);
        SetStatus(L"Name copied.");
    } else if (cmd == IDM_CTX_COPY_PATH) {
        SetClipboardTextW(p.path);
        SetStatus(L"Path copied.");
    } else if (cmd == IDM_CTX_COPY_PID) {
        SetClipboardTextW(std::to_wstring(p.pid));
        SetStatus(L"PID copied.");
    } else if (cmd == IDM_CTX_COPY_RULES) {
        std::vector<ProcessRule> mine;
        for (const auto& r : g_sv.Rules()) if (_wcsicmp(r.processName.c_str(), p.name.c_str()) == 0) mine.push_back(r);
        std::string json = ProcessSupervisor::RulesToJson(mine);
        SetClipboardTextW(Utf8ToW(json));
        SetStatus(std::to_wstring(mine.size()) + L" rule(s) copied.");
    } else if (cmd == IDM_CTX_PASTE_RULES) {
        std::wstring t = GetClipboardTextW();
        std::string u8;
        int n = WideCharToMultiByte(CP_UTF8, 0, t.c_str(), (int)t.size(), nullptr, 0, nullptr, nullptr);
        u8.resize(n);
        WideCharToMultiByte(CP_UTF8, 0, t.c_str(), (int)t.size(), &u8[0], n, nullptr, nullptr);
        std::vector<ProcessRule> parsed = ProcessSupervisor::RulesFromJson(u8);
        if (parsed.empty()) { SetStatus(L"The clipboard has no OpenLasso rules."); return; }
        SyncRulesFromList();
        auto& v = g_sv.Rules();
        for (auto& r : parsed) {
            r.processName = p.name;
            v.erase(std::remove_if(v.begin(), v.end(), [&](const ProcessRule& x) {
                return _wcsicmp(x.processName.c_str(), p.name.c_str()) == 0 && x.priorityClass == r.priorityClass;
            }), v.end());
            v.push_back(r);
        }
        FillRuleList();
        PersistNow();
        SetStatus(std::to_wstring(parsed.size()) + L" rule(s) pasted.");
    } else if (cmd == IDM_CTX_REMOVE_RULES) {
        SyncRulesFromList();
        auto& v = g_sv.Rules();
        v.erase(std::remove_if(v.begin(), v.end(), [&](const ProcessRule& x) {
            return _wcsicmp(x.processName.c_str(), p.name.c_str()) == 0;
        }), v.end());
        FillRuleList();
        PersistNow();
        SetStatus(L"Rules removed for " + p.name);
    }
}

static void ShowProcMenu() {
    HMENU m = BuildProcMenu();
    POINT pt;
    GetCursorPos(&pt);
    bool pauseNow = g_sv.Cfg().pauseOnRightClick && !g_paused;
    if (pauseNow) g_paused = true;
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_main, nullptr);
    DestroyMenu(m);
    if (pauseNow) g_paused = false;
    if (cmd) HandleProcMenu(cmd);
}

static bool IsElevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION te{};
    DWORD sz = 0;
    bool r = GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &sz) && te.TokenIsElevated;
    CloseHandle(tok);
    return r;
}

// ---------- shared actions ----------
static void ToggleStartup() {
    bool on = !g_startupOn;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    bool ok = ProcessSupervisor::SetAutostart(on, exe);
    g_startupOn = ProcessSupervisor::GetAutostart();
    CheckDlgButton(g_main, IDC_OPT_AUTOSTART, g_startupOn ? BST_CHECKED : BST_UNCHECKED);
    SetStatus(!ok ? L"Could not change the startup setting (run OpenLasso as administrator)."
                  : g_startupOn ? L"OpenLasso will start at sign-in." : L"Start at sign-in disabled.");
}
static void ApplyView() {
    const Settings& s = g_sv.Cfg();
    DWORD ex = LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER;
    if (s.gridlines) ex |= LVS_EX_GRIDLINES;
    if (s.tooltips) ex |= LVS_EX_INFOTIP;
    ListView_SetExtendedListViewStyle(g_proc, ex);
    SetWindowPos(g_main, s.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    COLORREF bg = s.darkTheme ? RGB(32, 34, 36) : RGB(255, 255, 255);
    COLORREF tx = s.darkTheme ? RGB(230, 230, 230) : RGB(0, 0, 0);
    for (HWND lv : { g_proc, g_rule, g_log }) {
        ListView_SetBkColor(lv, bg);
        ListView_SetTextBkColor(lv, bg);
        ListView_SetTextColor(lv, tx);
    }
    if (g_darkBrush) { DeleteObject(g_darkBrush); g_darkBrush = nullptr; }
    if (s.darkTheme) g_darkBrush = CreateSolidBrush(RGB(32, 34, 36));
    for (HWND lv : { g_proc, g_rule, g_log }) SetControlTheme(lv, s.darkTheme);
    SetDarkTitleBar(g_main, s.darkTheme);
    RebuildColumns();
    FillProcList();
    RedrawWindow(g_main, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    TrayUpdate();
}
static void SetEngineOverrides() {
    if (g_sv.Cfg().forced) g_sv.Cfg().autoApply = true;
}
static void DoResetColumns() {
    ModifySettings([](Settings& s) { s.columnsMask = kDefaultColumns; });
    ApplyView();
}
static void DoResetView() {
    ModifySettings([](Settings& s) {
        s.smooth = true; s.treeView = true; s.gridlines = false; s.icons = true; s.tooltips = true;
        s.alwaysOnTop = false; s.darkTheme = false; s.cpuUtility = true; s.columnsMask = kDefaultColumns;
        s.showEvents = s.showCpuLine = s.showCpuMeter = s.showRamMeter = s.showPlan = true;
        s.showRamLine = s.showGpuLine = s.showGpuMeter = true;
    });
    g_showGraph = true;
    ApplyView();
}

static void LoadProfile(const std::wstring& name) {
    std::wstring path = g_sv.ProfilePath(name);
    if (!g_sv.LoadConfigFrom(path)) { SetStatus(L"Could not load the profile."); return; }
    g_curProfile = name;
    WriteOptionsToControls(false);
    FillRuleList();
    ApplyView();
    SetStatus(L"Profile loaded: " + (name.empty() ? L"Default" : name));
}

static bool ChooseFileToSave(std::wstring& outPath, const wchar_t* defName) {
    wchar_t file[MAX_PATH] = {};
    wcscpy_s(file, defName);
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = L"JSON (*.json)\0*.json\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"json";
    ofn.Flags = OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&ofn)) return false;
    outPath = file;
    return true;
}
static bool ChooseFileToOpen(std::wstring& outPath) {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = L"JSON (*.json)\0*.json\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return false;
    outPath = file;
    return true;
}

static void PopulateProfiles(HMENU h) {
    while (DeleteMenu(h, 0, MF_BYPOSITION)) {}
    AppendMenuW(h, MF_STRING, IDM_PROFILE_BASE, L"Default");
    std::vector<std::wstring> names = ProcessSupervisor::ListProfiles(g_sv.ExeDir());
    for (size_t i = 0; i < names.size(); ++i)
        AppendMenuW(h, MF_STRING, IDM_PROFILE_BASE + 1 + (UINT)i, names[i].c_str());
}
static void PopulatePlans(HMENU h) {
    while (DeleteMenu(h, 0, MF_BYPOSITION)) {}
    g_plans = ProcessSupervisor::ListPowerPlans();
    std::wstring active;
    ProcessSupervisor::GetActivePowerPlan(active);
    for (int i = 0; i < (int)g_plans.size(); ++i) {
        AppendMenuW(h, MF_STRING, IDM_PLAN_BASE + i, g_plans[i].name.c_str());
        if (_wcsicmp(active.c_str(), g_plans[i].guid.c_str()) == 0) CheckMenuItem(h, IDM_PLAN_BASE + i, MF_BYCOMMAND | MF_CHECKED);
    }
    if (g_plans.empty()) AppendMenuW(h, MF_STRING | MF_GRAYED, 0, L"(no power plans found)");
}

// ---------- updates (GitHub releases via WinHTTP) ----------
static std::string HttpsGet(const std::wstring& host, const std::wstring& path) {
    std::string out;
    HINTERNET s = WinHttpOpen(L"OpenLasso", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!s) return out;
    HINTERNET c = WinHttpConnect(s, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (r && WinHttpSendRequest(r, L"User-Agent: OpenLasso\r\n", (DWORD)-1L, nullptr, 0, 0, 0) &&
        WinHttpReceiveResponse(r, nullptr)) {
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(r, &avail) && avail) {
            std::string buf(avail, '\0');
            DWORD got = 0;
            if (!WinHttpReadData(r, &buf[0], avail, &got) || !got) break;
            out.append(buf.data(), got);
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return out;
}
static void CheckForUpdates(bool silent) {
    const Settings& s = g_sv.Cfg();
    if (s.updateRepo.empty()) {
        if (!silent) MessageBoxW(g_main, L"Set \"update_repo\" (owner/repo) in your OpenLasso.json first.", L"Updates", MB_OK);
        return;
    }
    std::wstring path = L"/repos/" + s.updateRepo + (s.includeBetas ? L"/releases" : L"/releases/latest");
    std::string body = HttpsGet(L"api.github.com", path);
    size_t p = body.find("\"tag_name\":\"");
    if (p == std::string::npos) {
        if (!silent) MessageBoxW(g_main, L"Could not read the latest release.", L"Updates", MB_OK | MB_ICONWARNING);
        return;
    }
    p += 12;
    size_t e = body.find('"', p);
    std::wstring tag = Utf8ToW(body.substr(p, e - p));
    if (!tag.empty() && (tag[0] == L'v' || tag[0] == L'V')) tag = tag.substr(1);
    if (tag == kAppVersion) {
        if (!silent) MessageBoxW(g_main, L"You have the latest version.", L"Updates", MB_OK | MB_ICONINFORMATION);
    } else {
        std::wstring msg = L"A new version is available: " + tag + L"\n\nhttps://github.com/" + s.updateRepo + L"/releases";
        MessageBoxW(g_main, msg.c_str(), L"Updates", MB_OK | MB_ICONINFORMATION);
    }
}

// ---------- diagnostics ----------
static void CollectDiagnostics() {
    wchar_t up[MAX_PATH] = {};
    GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH);
    std::wstring file = std::wstring(up) + L"\\Desktop\\OpenLasso-diagnostics.txt";
    std::wstring t = L"OpenLasso " + std::wstring(kAppVersion) + L"\r\n";
    t += L"CPU: " + g_cpuName + L"\r\n";
    t += L"RAM: " + std::to_wstring((int)g_sv.MemoryTotalGB()) + L" GB, load " + std::to_wstring((int)g_sv.MemoryLoad()) + L"%\r\n";
    t += L"Processes: " + std::to_wstring(g_procs.size()) + L"\r\nRules: " + std::to_wstring(g_sv.Rules().size()) + L"\r\n";
    t += L"auto-throttle events: " + std::to_wstring(g_sv.ThrottleTotal()) + L"\r\n";
    t += L"GPU scheduling (WDDM): " + std::wstring(ProcessSupervisor::GpuSchedulingSupported() ? L"available" : L"not available") + L"\r\n";
    t += L"GPU counters: " + std::wstring(g_sv.GpuAvailable() ? L"available" : L"not available") + L"\r\n";
    t += L"Profile: " + (g_curProfile.empty() ? std::wstring(L"Default") : g_curProfile) + L"\r\n\r\nLast log lines:\r\n";
    const auto& log = g_sv.Log();
    size_t start = log.size() > 100 ? log.size() - 100 : 0;
    for (size_t i = start; i < log.size(); ++i) t += ProcessSupervisor::LogEntryText(log[i]) + L"\r\n";
    std::string u8;
    int n = WideCharToMultiByte(CP_UTF8, 0, t.c_str(), (int)t.size(), nullptr, 0, nullptr, nullptr);
    u8.resize(n);
    WideCharToMultiByte(CP_UTF8, 0, t.c_str(), (int)t.size(), &u8[0], n, nullptr, nullptr);
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(h, "\xEF\xBB\xBF", 3, &w, nullptr);
        WriteFile(h, u8.data(), (DWORD)u8.size(), &w, nullptr);
        CloseHandle(h);
        ShellExecuteW(g_main, L"open", L"notepad.exe", file.c_str(), nullptr, SW_SHOWNORMAL);
        SetStatus(L"Diagnostics saved on the desktop.");
    } else SetStatus(L"Could not write the diagnostics file.");
}

// ---------- menu state ----------
static int StateOf(UINT id, bool& on) {
    const Settings& s = g_sv.Cfg();
    on = false;
    switch (id) {
    case IDM_MAIN_ALLUSERS: on = s.manageAllUsers; return 1;
    case IDM_MAIN_THROTTLE: on = s.throttle; return 1;
    case IDM_MAIN_SMART: on = s.memoryOptimizer; return 1;
    case IDM_MAIN_IDLE: on = s.idleSaver; return 1;
    case IDM_MAIN_PERF: on = s.performance; return 1;
    case IDM_OPT_STARTUP: on = g_startupOn; return 1;
    case IDM_OPT_STARTMIN: on = s.startMinimized; return 1;
    case IDM_OPT_CLOSETRAY: on = s.closeToTray; return 1;
    case IDM_OPT_LOG: on = s.logEnabled; return 1;
    case IDM_OPT_BOOST: on = s.boost; return 1;
    case IDM_OPT_FORCED: on = s.forced; return 1;
    case IDM_VIEW_PAUSERC: on = s.pauseOnRightClick; return 1;
    case IDM_VIEW_CPUUTIL: on = s.cpuUtility; return 1;
    case IDM_VIEW_GRAPH: on = g_showGraph; return 1;
    case IDM_GC_EVENTS: on = s.showEvents; return 1;
    case IDM_GC_CPULINE: on = s.showCpuLine; return 1;
    case IDM_GC_CPUMETER: on = s.showCpuMeter; return 1;
    case IDM_GC_RAMMETER: on = s.showRamMeter; return 1;
    case IDM_GC_PLAN: on = s.showPlan; return 1;
    case IDM_GC_RAMLINE: on = s.showRamLine; return 1;
    case IDM_GC_GPULINE: on = s.showGpuLine; return 1;
    case IDM_GC_GPUMETER: on = s.showGpuMeter; return 1;
    case IDM_TRAY_ALWAYS: on = s.trayMode == 0; return 1;
    case IDM_TRAY_MIN: on = s.trayMode == 1; return 1;
    case IDM_TRAY_NEVER: on = s.trayMode == 2; return 1;
    case IDM_THEME_LIGHT: on = !s.darkTheme; return 1;
    case IDM_THEME_DARK: on = s.darkTheme; return 1;
    case IDM_VIEW_SMOOTH: on = s.smooth; return 1;
    case IDM_VIEW_TREE: on = s.treeView; return 1;
    case IDM_VIEW_GRID: on = s.gridlines; return 1;
    case IDM_VIEW_ICONS: on = s.icons; return 1;
    case IDM_VIEW_TIPS: on = s.tooltips; return 1;
    case IDM_VIEW_ONTOP: on = s.alwaysOnTop; return 1;
    case IDM_UPD_NEVER: on = s.updateCheck == 0; return 1;
    case IDM_UPD_DAILY: on = s.updateCheck == 1; return 1;
    case IDM_UPD_WEEKLY: on = s.updateCheck == 2; return 1;
    case IDM_UPD_BETA: on = s.includeBetas; return 1;
    case IDM_UPD_AUTO: on = s.updateCheck != 0; return 1;
    case IDM_AWAKE_OFF: on = !g_sv.KeepAwakeActive(); return 1;
    case IDM_AWAKE_INF: on = false; return 1;
    case IDM_OPT_NOTIFY: on = s.notifications; return 1;
    case IDM_OPT_DELEGATE: on = s.delegateToService; return 1;
    case IDM_OPT_THROTTLESEC + 0: on = s.throttleSeconds == 1; return 1;
    case IDM_OPT_THROTTLESEC + 1: on = s.throttleSeconds == 2; return 1;
    case IDM_OPT_THROTTLESEC + 2: on = s.throttleSeconds == 5; return 1;
    case IDM_OPT_THROTTLESEC + 3: on = s.throttleSeconds == 10; return 1;
    case IDM_MAIN_ENGINE: return 2;
    default: break;
    }
    if (id >= IDM_COL_BASE && id < IDM_COL_BASE + C_COUNT) {
        on = (s.columnsMask >> (id - IDM_COL_BASE)) & 1;
        return 1;
    }
    if (id >= IDM_LOGSIZE_BASE && id < IDM_LOGSIZE_BASE + 4) {
        on = s.maxLogKB == kLogSizesKB[id - IDM_LOGSIZE_BASE];
        return 1;
    }
    if (id >= IDM_PROFILE_BASE && id < IDM_PROFILE_BASE + 100) {
        on = (id == IDM_PROFILE_BASE) ? g_curProfile.empty() : false;
        return 1;
    }
    return 0;
}
static void UpdatePopup(HMENU h) {
    int n = GetMenuItemCount(h);
    for (int i = 0; i < n; ++i) {
        UINT id = GetMenuItemID(h, i);
        if (id == 0 || id == (UINT)-1) continue;
        bool on = false;
        int kind = StateOf(id, on);
        if (kind == 1) CheckMenuItem(h, i, MF_BYPOSITION | (on ? MF_CHECKED : MF_UNCHECKED));
        if (id == IDM_MAIN_ENGINE)
            ModifyMenuW(h, i, MF_BYPOSITION | MF_STRING, id, g_sv.EngineStopped() ? L"Resume Rules" : L"Pause Rules");
    }
    // profiles submenu: mark the current profile
    if (h == g_menuProfiles) {
        int n2 = GetMenuItemCount(h);
        for (int i = 0; i < n2; ++i) {
            UINT id = GetMenuItemID(h, i);
            if (id < IDM_PROFILE_BASE || id >= IDM_PROFILE_BASE + 100) continue;
            std::vector<wchar_t> buf(256, 0);
            GetMenuStringW(h, i, buf.data(), 256, MF_BYPOSITION);
            bool on = (id == IDM_PROFILE_BASE) ? g_curProfile.empty() : (g_curProfile == buf.data());
            CheckMenuItem(h, i, MF_BYPOSITION | (on ? MF_CHECKED : MF_UNCHECKED));
        }
    }
}

// ---------- main command dispatch ----------
static void DoCommand(int id);
static void DoMenuAction(int id);

static void DoCommand(int id) {
    switch (id) {
    case IDC_PAUSE:
        g_paused = !g_paused;
        SetWindowTextW(g_pause, g_paused ? L"Resume" : L"Pause");
        SetStatus(g_paused ? L"Paused." : L"Running.");
        break;
    case IDC_EDITRULES: ShowBottom(BOTTOM_RULES); break;
    case IDC_HIDEGRAPH:
    case IDM_VIEW_GRAPH: {
        g_showGraph = !g_showGraph;
        SetWindowTextW(g_hideGraph, g_showGraph ? L"Hide graph" : L"Show graph");
        RECT rc;
        GetClientRect(g_main, &rc);
        Layout(rc.right, rc.bottom);
        InvalidateRect(g_main, nullptr, TRUE);
        break;
    }
    case IDC_SUMMARY:
    case IDM_VIEW_SUMMARY:
        MessageBoxW(g_main, g_sv.SummaryText().c_str(), L"Auto-throttle Summary", MB_OK | MB_ICONINFORMATION);
        break;
    case IDC_VIEWLOG:
    case IDM_VIEW_LOG:
        ShowBottom(BOTTOM_LOG);
        break;
    case IDC_LOG_CLEAR:
    case IDM_OPT_LOGCLEAR:
        g_sv.ClearLog();
        RefreshLog();
        break;
    case IDC_DEL_RULE: {
        int i = ListView_GetNextItem(g_rule, -1, LVNI_SELECTED);
        if (i < 0) { SetStatus(L"Select a rule to delete."); break; }
        SyncRulesFromList();
        auto& v = g_sv.Rules();
        if (i < (int)v.size()) v.erase(v.begin() + i);
        FillRuleList();
        PersistNow();
        SetStatus(L"Rule deleted.");
        break;
    }
    case IDC_RUN_RULES: {
        SyncRulesFromList();
        SyncOptionsFromControls();
        g_procs = g_sv.EnumerateProcesses();
        SortProcs();
        int n = g_sv.ApplyAll(g_procs);
        FillProcList();
        SetStatus(L"Applied now: " + std::to_wstring(n) + L" change(s).");
        break;
    }
    case IDC_EXPORT:
    case IDM_FILE_EXPORT_RULES: {
        std::wstring p;
        if (ChooseFileToSave(p, L"OpenLasso-rules.json")) {
            SyncRulesFromList();
            SetStatus(g_sv.SaveRulesTo(p) ? L"Rules exported." : L"Export failed.");
        }
        break;
    }
    case IDC_IMPORT:
    case IDM_FILE_IMPORT_RULES: {
        std::wstring p;
        if (ChooseFileToOpen(p) && g_sv.LoadRulesFrom(p)) {
            FillRuleList();
            PersistNow();
            SetStatus(L"Imported " + std::to_wstring(g_sv.Rules().size()) + L" rule(s).");
        } else SetStatus(L"Could not import the rules.");
        break;
    }
    case IDM_FILE_EXPORT_ALL: {
        std::wstring p;
        if (ChooseFileToSave(p, L"OpenLasso-export.json")) {
            SyncRulesFromList();
            SyncOptionsFromControls();
            SetStatus(g_sv.SaveConfigTo(p) ? L"Configuration exported." : L"Export failed.");
        }
        break;
    }
    case IDM_FILE_IMPORT_ALL: {
        if (MessageBoxW(g_main, L"Importing replaces your current rules and settings. Continue?", L"Import",
                        MB_YESNO | MB_ICONQUESTION) != IDYES) break;
        std::wstring p;
        if (ChooseFileToOpen(p) && g_sv.LoadConfigFrom(p)) {
            WriteOptionsToControls(false);
            FillRuleList();
            ApplyView();
            SetStatus(L"Configuration imported.");
        } else SetStatus(L"Could not import the configuration.");
        break;
    }
    case IDC_SAVE:
    case IDM_FILE_SAVE:
        SetStatus(PersistNow() ? L"Configuration saved." : L"Could not save the configuration file.");
        break;
    case IDM_FILE_NEWPROFILE: {
        std::wstring name;
        if (!InputBox(L"New profile", L"Profile name (letters, digits, spaces):", name) || name.empty()) break;
        for (wchar_t bad : std::wstring(L"\\/:*?\"<>|")) name.erase(std::remove(name.begin(), name.end(), bad), name.end());
        SyncRulesFromList();
        SyncOptionsFromControls();
        if (g_sv.SaveConfigTo(g_sv.ProfilePath(name))) {
            g_curProfile = name;
            SetStatus(L"Profile saved: " + name);
        } else SetStatus(L"Could not create the profile.");
        break;
    }
    case IDM_FILE_EDIT:
        PersistNow();
        ShellExecuteW(g_main, L"open", L"notepad.exe", g_sv.ProfilePath(g_curProfile).c_str(), nullptr, SW_SHOWNORMAL);
        break;
    case IDM_FILE_LOGFOLDER: {
        BROWSEINFOW bi = {};
        bi.hwndOwner = g_main;
        bi.lpszTitle = L"Choose the folder for OpenLasso-log.csv";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
        if (pidl) {
            wchar_t p[MAX_PATH] = {};
            if (SHGetPathFromIDListW(pidl, p)) {
                ModifySettings([&](Settings& s) { s.logFolder = p; });
                SetStatus(L"Log folder: " + std::wstring(p));
            }
            CoTaskMemFree(pidl);
        }
        break;
    }
    case IDC_OPT_AUTOSTART:
    case IDM_OPT_STARTUP:
        ToggleStartup();
        break;
    case IDM_OPT_STARTMIN: ToggleSetting(&Settings::startMinimized); break;
    case IDM_OPT_CLOSETRAY: ToggleSetting(&Settings::closeToTray); break;
    case IDM_OPT_LOG: ToggleSetting(&Settings::logEnabled); break;
    case IDM_OPT_BOOST: ToggleSetting(&Settings::boost); break;
    case IDM_OPT_FORCED: ToggleSetting(&Settings::forced); break;
    case IDM_MAIN_ALLUSERS: ToggleSetting(&Settings::manageAllUsers); ProcsTick(); break;
    case IDM_MAIN_THROTTLE: ToggleSetting(&Settings::throttle); break;
    case IDM_MAIN_SMART: ToggleSetting(&Settings::memoryOptimizer); break;
    case IDM_MAIN_IDLE: ToggleSetting(&Settings::idleSaver); break;
    case IDM_MAIN_PERF: ToggleSetting(&Settings::performance); break;
    case IDM_MAIN_ENGINE:
        g_sv.SetEngineStopped(!g_sv.EngineStopped());
        SetStatus(g_sv.EngineStopped() ? L"Rules paused: no rule or auto-throttle changes." : L"Rules running.");
        break;
    case IDM_OPT_FREEMEM: {
        int n = g_sv.FreeBackgroundMemory(g_procs);
        SetStatus(L"Freed memory of " + std::to_wstring(n) + L" processes.");
        break;
    }
    case IDM_OPT_SETTINGS: ShowBottom(BOTTOM_OPTIONS); break;
    case IDM_OPT_RULES: ShowBottom(BOTTOM_RULES); break;
    case IDM_OPT_RESET:
        if (MessageBoxW(g_main, L"Reset all rules and settings to their defaults?", L"Reset configuration",
                        MB_YESNO | MB_ICONWARNING) != IDYES) break;
        g_sv.ResetConfig();
        WriteOptionsToControls(false);
        FillRuleList();
        ApplyView();
        PersistNow();
        SetStatus(L"Configuration reset.");
        break;
    case IDC_SVC_INST:
        PersistNow();
        SetStatus(g_sv.InstallService() ? L"Service installed (starts automatically)." : L"Could not install the service.");
        break;
    case IDC_SVC_UNINST:
        SetStatus(g_sv.UninstallService() ? L"Service removed." : L"Could not remove the service.");
        break;
    case IDC_SVC_START:
        SetStatus(g_sv.StartSvc() ? L"Service started." : L"Could not start the service.");
        break;
    case IDC_SVC_STOP:
        SetStatus(g_sv.StopSvc() ? L"Service stopped." : L"Could not stop the service.");
        break;
    default:
        DoMenuAction(id);
        break;
    }
}

static void DoMenuAction(int id) {
    if (id >= IDM_PROFILE_BASE && id < IDM_PROFILE_BASE + 100) {
        if (id == IDM_PROFILE_BASE) { LoadProfile(L""); return; }
        std::vector<std::wstring> names = ProcessSupervisor::ListProfiles(g_sv.ExeDir());
        int i = id - IDM_PROFILE_BASE - 1;
        if (i >= 0 && i < (int)names.size()) LoadProfile(names[i]);
        return;
    }
    if (id >= IDM_PLAN_BASE && id < IDM_PLAN_BASE + 100) {
        int i = id - IDM_PLAN_BASE;
        g_plans = ProcessSupervisor::ListPowerPlans();
        if (i < (int)g_plans.size()) {
            bool ok = ProcessSupervisor::SetActivePowerPlan(g_plans[i].guid);
            SetStatus(ok ? L"Active power profile: " + g_plans[i].name : L"Could not set the power profile.");
        }
        return;
    }
    if (id >= IDM_LOGSIZE_BASE && id < IDM_LOGSIZE_BASE + 4) {
        int kb = kLogSizesKB[id - IDM_LOGSIZE_BASE];
        ModifySettings([kb](Settings& s) { s.maxLogKB = kb; });
        SetStatus(L"Log file is rotated at " + std::to_wstring(kb) + L" KB.");
        return;
    }
    if (id >= IDM_COL_BASE && id < IDM_COL_BASE + C_COUNT) {
        int c = id - IDM_COL_BASE;
        ModifySettings([c](Settings& s) { s.columnsMask ^= (1u << c); });
        ApplyView();
        return;
    }
    switch (id) {
    case IDM_MAIN_ELEVATE:
        if (IsElevated()) { SetStatus(L"OpenLasso is already running as administrator."); break; }
        ShellExecuteW(g_main, L"runas", (g_sv.ExeDir() + L"OpenLasso.exe").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        DestroyWindow(g_main);
        break;
    case IDM_MAIN_HIDE: ShowWindow(g_main, SW_HIDE); TrayUpdate(); break;
    case IDM_MAIN_EXIT: DestroyWindow(g_main); break;
    case IDM_TRAY_SHOW: ShowMain(); break;
    case IDM_TRAY_PAUSE: DoCommand(IDC_PAUSE); break;
    case IDM_AWAKE_OFF: g_sv.SetKeepAwake(0); SetStatus(L"Stay awake: off."); break;
    case IDM_AWAKE_1H: g_sv.SetKeepAwake(60); SetStatus(L"Keeping the PC awake for 1 hour."); break;
    case IDM_AWAKE_2H: g_sv.SetKeepAwake(120); SetStatus(L"Keeping the PC awake for 2 hours."); break;
    case IDM_AWAKE_4H: g_sv.SetKeepAwake(240); SetStatus(L"Keeping the PC awake for 4 hours."); break;
    case IDM_AWAKE_INF: g_sv.SetKeepAwake(-1); SetStatus(L"Keeping the PC awake indefinitely."); break;
    case IDM_GC_EVENTS: ToggleSetting(&Settings::showEvents); break;
    case IDM_GC_CPULINE: ToggleSetting(&Settings::showCpuLine); break;
    case IDM_GC_CPUMETER: ToggleSetting(&Settings::showCpuMeter); break;
    case IDM_GC_RAMMETER: ToggleSetting(&Settings::showRamMeter); break;
    case IDM_GC_RAMLINE: ToggleSetting(&Settings::showRamLine); break;
    case IDM_GC_GPULINE: ToggleSetting(&Settings::showGpuLine); break;
    case IDM_GC_GPUMETER: ToggleSetting(&Settings::showGpuMeter); break;
    case IDM_PERF_CREATE: {
        std::wstring g;
        bool ok = g_sv.EnsurePerformancePlan(g, true);
        SetStatus(ok ? L"Power plan ready: " + std::wstring(ProcessSupervisor::PerfPlanName())
                     : std::wstring(L"Could not create the power plan."));
        break;
    }
    case IDM_GC_PLAN: ToggleSetting(&Settings::showPlan); break;
    case IDM_VIEW_PAUSERC: ToggleSetting(&Settings::pauseOnRightClick); break;
    case IDM_VIEW_CPUUTIL: ToggleSetting(&Settings::cpuUtility); ApplyView(); break;
    case IDM_VIEW_TREE: ToggleSetting(&Settings::treeView); ProcsTick(); break;
    case IDM_VIEW_GRID: ToggleSetting(&Settings::gridlines); ApplyView(); break;
    case IDM_VIEW_ICONS: ToggleSetting(&Settings::icons); ProcsTick(); break;
    case IDM_VIEW_TIPS: ToggleSetting(&Settings::tooltips); ApplyView(); break;
    case IDM_VIEW_ONTOP: ToggleSetting(&Settings::alwaysOnTop); ApplyView(); break;
    case IDM_VIEW_SMOOTH: ToggleSetting(&Settings::smooth); break;
    case IDM_THEME_LIGHT: ModifySettings([](Settings& s) { s.darkTheme = false; }); ApplyView(); break;
    case IDM_THEME_DARK: ModifySettings([](Settings& s) { s.darkTheme = true; }); ApplyView(); break;
    case IDM_TRAY_ALWAYS: ModifySettings([](Settings& s) { s.trayMode = 0; }); TrayUpdate(); break;
    case IDM_TRAY_MIN: ModifySettings([](Settings& s) { s.trayMode = 1; }); TrayUpdate(); break;
    case IDM_TRAY_NEVER: ModifySettings([](Settings& s) { s.trayMode = 2; }); TrayUpdate(); break;
    case IDM_RESET_COLS: DoResetColumns(); break;
    case IDM_RESET_VIEW: DoResetView(); break;
    case IDM_TOOL_CFG: ShellExecuteW(g_main, L"open", g_sv.ExeDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case IDM_TOOL_LOG: ShellExecuteW(g_main, L"open", L"notepad.exe", g_sv.LogFilePath().c_str(), nullptr, SW_SHOWNORMAL); break;
    case IDM_TOOL_TM: ShellExecuteW(g_main, L"open", L"taskmgr.exe", nullptr, nullptr, SW_SHOWNORMAL); break;
    case IDM_TOOL_SVC: ShellExecuteW(g_main, L"open", L"services.msc", nullptr, nullptr, SW_SHOWNORMAL); break;
    case IDM_UPD_CHECK: CheckForUpdates(false); break;
    case IDM_UPD_NEVER: ModifySettings([](Settings& s) { s.updateCheck = 0; }); break;
    case IDM_UPD_DAILY: ModifySettings([](Settings& s) { s.updateCheck = 1; }); break;
    case IDM_UPD_WEEKLY: ModifySettings([](Settings& s) { s.updateCheck = 2; }); break;
    case IDM_UPD_BETA: ToggleSetting(&Settings::includeBetas); break;
    case IDM_UPD_AUTO: ModifySettings([](Settings& s) { s.updateCheck = s.updateCheck ? 0 : 1; }); break;
    case IDM_HELP_DOC: {
        std::wstring doc = g_sv.ExeDir() + L"README.md";
        if (GetFileAttributesW(doc.c_str()) != INVALID_FILE_ATTRIBUTES)
            ShellExecuteW(g_main, L"open", doc.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        else if (!g_sv.Cfg().updateRepo.empty())
            ShellExecuteW(g_main, L"open", (L"https://github.com/" + g_sv.Cfg().updateRepo).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        else SetStatus(L"README.md is not next to OpenLasso.exe. Set update_repo to open the project page.");
        break;
    }
    case IDM_HELP_WEB:
        if (g_sv.Cfg().updateRepo.empty()) SetStatus(L"Set update_repo in the settings to open your project page.");
        else ShellExecuteW(g_main, L"open", (L"https://github.com/" + g_sv.Cfg().updateRepo).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case IDM_OPT_NOTIFY: ToggleSetting(&Settings::notifications); break;
    case IDM_OPT_DELEGATE: ToggleSetting(&Settings::delegateToService); break;
    case IDM_OPT_THROTTLESEC + 0: ModifySettings([](Settings& s) { s.throttleSeconds = 1; }); break;
    case IDM_OPT_THROTTLESEC + 1: ModifySettings([](Settings& s) { s.throttleSeconds = 2; }); break;
    case IDM_OPT_THROTTLESEC + 2: ModifySettings([](Settings& s) { s.throttleSeconds = 5; }); break;
    case IDM_OPT_THROTTLESEC + 3: ModifySettings([](Settings& s) { s.throttleSeconds = 10; }); break;
    case IDM_VIEW_REPORT:
        ShellExecuteW(g_main, L"open", L"notepad.exe", g_sv.ThrottleReportPath().c_str(), nullptr, SW_SHOWNORMAL);
        break;
    case IDM_HELP_DIAG: CollectDiagnostics(); break;
    case IDM_ABOUT:
        MessageBoxW(g_main, (L"OpenLasso " + std::wstring(kAppVersion) +
                             L"\n\nOpen-source process priority, affinity and auto-throttle manager.\nby watergamer111\n"
                             L"Uses documented Windows APIs; a few advanced functions use ntdll or the Windows shell.")
                            .c_str(), L"About OpenLasso", MB_OK | MB_ICONINFORMATION);
        break;
    default:
        // context-menu commands are handled by HandleProcMenu
        if (id >= IDM_CTX_PRIO && id < IDM_CTX_PLAN + 100) HandleProcMenu(id);
        break;
    }
}

// ---------- menu bar ----------
static void Add(HMENU m, UINT id, const wchar_t* text, UINT flags = MF_STRING) {
    AppendMenuW(m, flags, id, text);
}
static HMENU MakeAwakeMenu() {
    HMENU h = CreatePopupMenu();
    Add(h, IDM_AWAKE_OFF, L"Off");
    Add(h, IDM_AWAKE_1H, L"For 1 hour");
    Add(h, IDM_AWAKE_2H, L"For 2 hours");
    Add(h, IDM_AWAKE_4H, L"For 4 hours");
    Add(h, IDM_AWAKE_INF, L"Indefinitely");
    return h;
}
static void BuildMenuBar(HWND hwnd) {
    HMENU bar = CreateMenu();

    HMENU mMain = CreatePopupMenu();
    Add(mMain, IDM_MAIN_HIDE, L"Hide OpenLasso");
    Add(mMain, IDM_MAIN_ALLUSERS, L"Manage Processes of All Users");
    Add(mMain, IDM_MAIN_ELEVATE, L"Elevate Now");
    AppendMenuW(mMain, MF_SEPARATOR, 0, nullptr);
    g_menuPlansMain = CreatePopupMenu();
    AppendMenuW(mMain, MF_POPUP, (UINT_PTR)g_menuPlansMain, L"Active Power Profile");
    g_menuAwake = MakeAwakeMenu();
    AppendMenuW(mMain, MF_POPUP, (UINT_PTR)g_menuAwake, L"Stay Awake");
    AppendMenuW(mMain, MF_SEPARATOR, 0, nullptr);
    Add(mMain, IDM_MAIN_THROTTLE, L"Auto-throttle Enabled");
    Add(mMain, IDM_MAIN_SMART, L"Memory Trim Enabled");
    Add(mMain, IDM_MAIN_IDLE, L"Idle power saver Enabled");
    Add(mMain, IDM_MAIN_PERF, L"High-Power Mode Enabled");
    AppendMenuW(mMain, MF_SEPARATOR, 0, nullptr);
    Add(mMain, IDM_MAIN_ENGINE, L"Pause Rules");
    Add(mMain, IDM_MAIN_EXIT, L"Exit OpenLasso");

    HMENU mFile = CreatePopupMenu();
    Add(mFile, IDM_FILE_SAVE, L"Save configuration");
    g_menuProfiles = CreatePopupMenu();
    AppendMenuW(mFile, MF_POPUP, (UINT_PTR)g_menuProfiles, L"Config Profile");
    Add(mFile, IDM_FILE_NEWPROFILE, L"Save current as new profile...");
    AppendMenuW(mFile, MF_SEPARATOR, 0, nullptr);
    Add(mFile, IDM_FILE_IMPORT_RULES, L"Import Rules...");
    Add(mFile, IDM_FILE_EXPORT_RULES, L"Export Rules...");
    AppendMenuW(mFile, MF_SEPARATOR, 0, nullptr);
    Add(mFile, IDM_FILE_IMPORT_ALL, L"Import Entire Configuration...");
    Add(mFile, IDM_FILE_EXPORT_ALL, L"Export Entire Configuration...");
    AppendMenuW(mFile, MF_SEPARATOR, 0, nullptr);
    Add(mFile, IDM_FILE_EDIT, L"Manually Edit Configuration");
    Add(mFile, IDM_FILE_LOGFOLDER, L"Choose Log Folder...");

    HMENU mView = CreatePopupMenu();
    g_menuColumns = CreatePopupMenu();
    for (int c = 1; c < C_COUNT; ++c) Add(g_menuColumns, IDM_COL_BASE + c, kColTitle[c]);
    AppendMenuW(mView, MF_POPUP, (UINT_PTR)g_menuColumns, L"Choose Columns");
    Add(mView, IDM_VIEW_LOG, L"View Log...");
    Add(mView, IDM_VIEW_SUMMARY, L"Open Auto-throttle Summary...");
    Add(mView, IDM_VIEW_REPORT, L"Open Auto-throttle report (CSV)...");
    AppendMenuW(mView, MF_SEPARATOR, 0, nullptr);
    Add(mView, IDM_VIEW_PAUSERC, L"Pause on Right-Click");
    Add(mView, IDM_VIEW_CPUUTIL, L"Show CPU Utility % instead of CPU Time %");
    AppendMenuW(mView, MF_SEPARATOR, 0, nullptr);
    Add(mView, IDM_VIEW_GRAPH, L"Show Graph");
    HMENU gc = CreatePopupMenu();
    Add(gc, IDM_GC_EVENTS, L"auto-throttle events");
    Add(gc, IDM_GC_CPULINE, L"CPU load line");
    Add(gc, IDM_GC_RAMLINE, L"RAM load line");
    Add(gc, IDM_GC_GPULINE, L"GPU load line");
    Add(gc, IDM_GC_CPUMETER, L"CPU meter");
    Add(gc, IDM_GC_RAMMETER, L"RAM meter");
    Add(gc, IDM_GC_GPUMETER, L"GPU meter");
    Add(gc, IDM_GC_PLAN, L"Power plan name");
    AppendMenuW(mView, MF_POPUP, (UINT_PTR)gc, L"Graph Components");
    HMENU tray = CreatePopupMenu();
    Add(tray, IDM_TRAY_ALWAYS, L"Always show");
    Add(tray, IDM_TRAY_MIN, L"Show when hidden or minimized");
    Add(tray, IDM_TRAY_NEVER, L"Never (while the window is visible)");
    AppendMenuW(mView, MF_POPUP, (UINT_PTR)tray, L"Tray Icon");
    HMENU colors = CreatePopupMenu();
    Add(colors, IDM_THEME_LIGHT, L"Light");
    Add(colors, IDM_THEME_DARK, L"Dark");
    AppendMenuW(mView, MF_POPUP, (UINT_PTR)colors, L"Colors");
    HMENU eff = CreatePopupMenu();
    Add(eff, IDM_VIEW_SMOOTH, L"Smooth graph");
    AppendMenuW(mView, MF_POPUP, (UINT_PTR)eff, L"Effects");
    HMENU reset = CreatePopupMenu();
    Add(reset, IDM_RESET_COLS, L"Reset columns");
    Add(reset, IDM_RESET_VIEW, L"Reset view");
    AppendMenuW(mView, MF_POPUP, (UINT_PTR)reset, L"Reset");
    AppendMenuW(mView, MF_SEPARATOR, 0, nullptr);
    Add(mView, IDM_VIEW_TREE, L"Tree View");
    Add(mView, IDM_VIEW_GRID, L"Show Gridlines");
    Add(mView, IDM_VIEW_ICONS, L"Show Process Icons");
    Add(mView, IDM_VIEW_TIPS, L"Show Process Tooltips");
    Add(mView, IDM_VIEW_ONTOP, L"Always on Top");

    HMENU mOpt = CreatePopupMenu();
    HMENU general = CreatePopupMenu();
    Add(general, IDM_OPT_STARTUP, L"Start with Windows");
    Add(general, IDM_OPT_STARTMIN, L"Start minimized to tray");
    Add(general, IDM_OPT_CLOSETRAY, L"Close button hides to tray");
    AppendMenuW(mOpt, MF_POPUP, (UINT_PTR)general, L"General");
    HMENU logm = CreatePopupMenu();
    Add(logm, IDM_OPT_LOG, L"Enable log file");
    Add(logm, IDM_OPT_LOGCLEAR, L"Clear log");
    Add(logm, IDM_FILE_LOGFOLDER, L"Choose log folder...");
    HMENU logsz = CreatePopupMenu();
    Add(logsz, IDM_LOGSIZE_BASE + 0, L"256 KB");
    Add(logsz, IDM_LOGSIZE_BASE + 1, L"1 MB");
    Add(logsz, IDM_LOGSIZE_BASE + 2, L"4 MB");
    Add(logsz, IDM_LOGSIZE_BASE + 3, L"16 MB");
    AppendMenuW(logm, MF_POPUP, (UINT_PTR)logsz, L"Maximum log size");
    AppendMenuW(mOpt, MF_POPUP, (UINT_PTR)logm, L"Log");
    HMENU cpum = CreatePopupMenu();
    Add(cpum, IDM_MAIN_THROTTLE, L"Auto-throttle Enabled");
    Add(cpum, IDM_OPT_BOOST, L"Active window boost");
    Add(cpum, IDM_OPT_SETTINGS, L"Auto-throttle thresholds...");
    Add(cpum, IDM_OPT_THROTTLESEC + 0, L"Auto-throttle duration: 1 s");
    Add(cpum, IDM_OPT_THROTTLESEC + 1, L"Auto-throttle duration: 2 s");
    Add(cpum, IDM_OPT_THROTTLESEC + 2, L"Auto-throttle duration: 5 s");
    Add(cpum, IDM_OPT_THROTTLESEC + 3, L"Auto-throttle duration: 10 s");
    Add(cpum, IDM_OPT_NOTIFY, L"Notifications (tray balloons)");
    Add(cpum, IDM_OPT_DELEGATE, L"Let the Windows service apply rules when it runs");
    AppendMenuW(mOpt, MF_POPUP, (UINT_PTR)cpum, L"CPU");
    HMENU memm = CreatePopupMenu();
    Add(memm, IDM_OPT_FREEMEM, L"Trim memory now");
    Add(memm, IDM_MAIN_SMART, L"Memory Trim Enabled");
    AppendMenuW(mOpt, MF_POPUP, (UINT_PTR)memm, L"Memory");
    HMENU powm = CreatePopupMenu();
    g_menuPlansOpt = CreatePopupMenu();
    AppendMenuW(powm, MF_POPUP, (UINT_PTR)g_menuPlansOpt, L"Active Power Profile");
    g_menuAwake2 = MakeAwakeMenu();
    AppendMenuW(powm, MF_POPUP, (UINT_PTR)g_menuAwake2, L"Stay Awake");
    Add(powm, IDM_MAIN_IDLE, L"Idle power saver Enabled");
    Add(powm, IDM_MAIN_PERF, L"High-Power Mode Enabled");
    Add(powm, IDM_PERF_CREATE, L"Create / update the OpenLasso High Power plan");
    AppendMenuW(mOpt, MF_POPUP, (UINT_PTR)powm, L"Power");
    HMENU iom = CreatePopupMenu();
    Add(iom, IDM_COL_BASE + C_IO, L"Show I/O priority column");
    AppendMenuW(mOpt, MF_POPUP, (UINT_PTR)iom, L"I/O");
    HMENU ctrl = CreatePopupMenu();
    Add(ctrl, IDM_MAIN_ENGINE, L"Pause Rules");
    Add(ctrl, IDM_OPT_FORCED, L"Forced Mode");
    AppendMenuW(mOpt, MF_POPUP, (UINT_PTR)ctrl, L"Control");
    HMENU tools = CreatePopupMenu();
    Add(tools, IDM_TOOL_CFG, L"Open configuration folder");
    Add(tools, IDM_TOOL_LOG, L"Open log file");
    Add(tools, IDM_TOOL_TM, L"Task Manager");
    Add(tools, IDM_TOOL_SVC, L"Services");
    AppendMenuW(mOpt, MF_POPUP, (UINT_PTR)tools, L"Tools");
    AppendMenuW(mOpt, MF_SEPARATOR, 0, nullptr);
    Add(mOpt, IDM_OPT_RULES, L"View All Rules");
    Add(mOpt, IDM_OPT_RESET, L"Reset Configuration");

    HMENU mUpd = CreatePopupMenu();
    Add(mUpd, IDM_UPD_CHECK, L"Check Now");
    AppendMenuW(mUpd, MF_SEPARATOR, 0, nullptr);
    HMENU every = CreatePopupMenu();
    Add(every, IDM_UPD_NEVER, L"Never");
    Add(every, IDM_UPD_DAILY, L"Daily");
    Add(every, IDM_UPD_WEEKLY, L"Weekly");
    AppendMenuW(mUpd, MF_POPUP, (UINT_PTR)every, L"Check Every...");
    Add(mUpd, IDM_UPD_BETA, L"Include Betas");
    Add(mUpd, IDM_UPD_AUTO, L"Automatic Updates");

    HMENU mHelp = CreatePopupMenu();
    Add(mHelp, IDM_HELP_DOC, L"View Documentation");
    Add(mHelp, IDM_HELP_WEB, L"Visit Project Page");
    Add(mHelp, IDM_HELP_DIAG, L"Collect Diagnostic Information...");
    AppendMenuW(mHelp, MF_SEPARATOR, 0, nullptr);
    Add(mHelp, IDM_ABOUT, L"About OpenLasso");

    AppendMenuW(bar, MF_POPUP, (UINT_PTR)mMain, L"Main");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)mFile, L"File");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)mView, L"View");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)mOpt, L"Options");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)mUpd, L"Updates");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)mHelp, L"Help");
    SetMenu(hwnd, bar);
}

// ---------- window procedure ----------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_main = hwnd;
        g_titleFont = CreateFontW(-15, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_smallFont = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        BuildMenuBar(hwnd);

        wchar_t cpu[256] = {};
        DWORD cb = sizeof(cpu);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                         L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, cpu, &cb) == ERROR_SUCCESS)
            g_cpuName = StripText(cpu);

        g_vtab = Make(WC_TABCONTROLW, L"", 0, IDC_VTAB);
        TCITEMW ti = {};
        ti.mask = TCIF_TEXT;
        ti.pszText = const_cast<LPWSTR>(L"All processes");
        TabCtrl_InsertItem(g_vtab, TAB_ALL, &ti);
        ti.pszText = const_cast<LPWSTR>(L"Active processes");
        TabCtrl_InsertItem(g_vtab, TAB_ACTIVE, &ti);
        g_pause = Make(L"BUTTON", L"Pause", BS_PUSHBUTTON, IDC_PAUSE);
        g_editRules = Make(L"BUTTON", L"Edit Rules", BS_PUSHBUTTON, IDC_EDITRULES);
        g_hideGraph = Make(L"BUTTON", L"Hide graph", BS_PUSHBUTTON, IDC_HIDEGRAPH);
        g_filter = Make(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER, IDC_FILTER);

        g_proc = Make(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, IDC_PROCLIST);
        g_imgList = ImageList_Create(16, 16, ILC_COLOR32 | ILC_MASK, 0, 64);
        ListView_SetImageList(g_proc, g_imgList, LVSIL_SMALL);

        g_btab = Make(WC_TABCONTROLW, L"", 0, IDC_BTAB);
        const wchar_t* bnames[] = { L"Actions log", L"Rules", L"Options", L"Threads" };
        for (int i = 0; i < BOTTOM_COUNT; ++i) {
            ti.pszText = const_cast<LPWSTR>(bnames[i]);
            TabCtrl_InsertItem(g_btab, i, &ti);
        }
        Make(L"BUTTON", L"Summary", BS_PUSHBUTTON, IDC_SUMMARY);
        Make(L"BUTTON", L"View Log", BS_PUSHBUTTON, IDC_VIEWLOG);
        Make(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER, IDC_LOGFILTER);

        g_log = Make(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | WS_BORDER | WS_TABSTOP, IDC_LOG);
        ListView_SetExtendedListViewStyle(g_log, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        AddColumn(g_log, 0, L"Time", 140);
        AddColumn(g_log, 1, L"Computer", 110);
        AddColumn(g_log, 2, L"Action", 360);
        AddColumn(g_log, 3, L"Process", 140);
        AddColumn(g_log, 4, L"PID", 60);
        AddColumn(g_log, 5, L"Path", 280);
        AddColumn(g_log, 6, L"Command line", 420);
        Make(L"BUTTON", L"Clear log", BS_PUSHBUTTON, IDC_LOG_CLEAR);
        g_thList = Make(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, IDC_THREADLIST);
        ListView_SetExtendedListViewStyle(g_thList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        AddColumn(g_thList, 0, L"Thread ID", 90);
        AddColumn(g_thList, 1, L"Priority", 130);
        AddColumn(g_thList, 2, L"CPU (%)", 90);
        AddColumn(g_thList, 3, L"CPU time", 110);
        Make(L"STATIC", L"Threads of the selected process. Right-click a thread for priority, affinity, boost and suspend.", 0, IDC_TH_HINT);

        g_rule = Make(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, IDC_RULELIST);
        ListView_SetExtendedListViewStyle(g_rule, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER);
        AddColumn(g_rule, 0, L"Process", 200);
        AddColumn(g_rule, 1, L"Priority", 140);
        AddColumn(g_rule, 2, L"Affinity", 100);
        AddColumn(g_rule, 3, L"Memory", 120);
        AddColumn(g_rule, 4, L"Efficiency", 90);
        AddColumn(g_rule, 5, L"Power plan", 200);
        AddColumn(g_rule, 6, L"CPU sets", 110);
        AddColumn(g_rule, 7, L"GPU priority", 120);
        AddColumn(g_rule, 8, L"GPU preference", 130);
        AddColumn(g_rule, 9, L"Sleep / performance", 160);
        AddColumn(g_rule, 10, L"Keep running", 110);
        AddColumn(g_rule, 11, L"Restart arguments", 260);
        Make(L"BUTTON", L"Delete rule", BS_PUSHBUTTON, IDC_DEL_RULE);
        Make(L"BUTTON", L"Apply rules now", BS_PUSHBUTTON, IDC_RUN_RULES);
        Make(L"BUTTON", L"Export...", BS_PUSHBUTTON, IDC_EXPORT);
        Make(L"BUTTON", L"Import...", BS_PUSHBUTTON, IDC_IMPORT);

        Make(L"BUTTON", L"Auto-throttle: lower background CPU hogs when the system is busy", BS_AUTOCHECKBOX, IDC_OPT_THROTTLE);
        Make(L"STATIC", L"Start when total CPU load reaches (%):", 0, IDC_LBL_THRESH);
        Make(L"EDIT", L"", ES_NUMBER | WS_BORDER, IDC_OPT_THRESH);
        Make(L"STATIC", L"Only lower processes using at least (% of CPU):", 0, IDC_LBL_PROC);
        Make(L"EDIT", L"", ES_NUMBER | WS_BORDER, IDC_OPT_PROC);
        Make(L"BUTTON", L"Active window boost: raise the active window", BS_AUTOCHECKBOX, IDC_OPT_BOOST);
        Make(L"BUTTON", L"Apply rules every 1 s while this window is open", BS_AUTOCHECKBOX, IDC_OPT_AUTO);
        Make(L"BUTTON", L"Start with Windows (at sign-in, elevated)", BS_AUTOCHECKBOX, IDC_OPT_AUTOSTART);
        Make(L"BUTTON", L"Forced mode (re-apply rules every second)", BS_AUTOCHECKBOX, IDC_OPT_FORCED);
        Make(L"BUTTON", L"Memory Trim (free memory of idle processes when memory is high)", BS_AUTOCHECKBOX, IDC_OPT_SMART);
        Make(L"BUTTON", L"Idle power saver (saver plan after idle time)", BS_AUTOCHECKBOX, IDC_OPT_IDLE);
        Make(L"BUTTON", L"High-power mode (high performance plan when busy)", BS_AUTOCHECKBOX, IDC_OPT_PERF);
        Make(L"BUTTON", L"Close button hides OpenLasso to the tray", BS_AUTOCHECKBOX, IDC_OPT_TRAY);
        Make(L"BUTTON", L"GPU priority: fall back to GPU preference", BS_AUTOCHECKBOX, IDC_OPT_GPUFB);
        Make(L"BUTTON", L"High-power mode: own OpenLasso plan", BS_AUTOCHECKBOX, IDC_OPT_PERFPLAN);
        Make(L"STATIC", L"Never adjust these processes (one name per line):", 0, IDC_LBL_EXCL);
        Make(L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_BORDER, IDC_OPT_EXCL);
        Make(L"STATIC", L"Windows service (keeps applying rules with the window closed):", 0, IDC_LBL_SVC);
        Make(L"BUTTON", L"Install service", BS_PUSHBUTTON, IDC_SVC_INST);
        Make(L"BUTTON", L"Remove service", BS_PUSHBUTTON, IDC_SVC_UNINST);
        Make(L"BUTTON", L"Start", BS_PUSHBUTTON, IDC_SVC_START);
        Make(L"BUTTON", L"Stop", BS_PUSHBUTTON, IDC_SVC_STOP);

        g_status = Make(L"STATIC", L"", SS_ENDELLIPSIS, IDC_STATUS);
        Make(L"BUTTON", L"Save", BS_PUSHBUTTON, IDC_SAVE);

        InstallDarkSubclasses(hwnd, { g_proc, g_rule, g_log });
        g_sv.LoadConfig();
        if (g_startMinimized) { g_sv.Cfg().autoApply = true; g_sv.Cfg().startMinimized = true; }
        WriteOptionsToControls(true);
        FillRuleList();
        RebuildColumns();
        ApplyView();
        ShowBottom(BOTTOM_LOG);
        SetTimer(hwnd, TIMER_SAMPLE, SAMPLE_MS, nullptr);
        SetTimer(hwnd, TIMER_PROCS, PROCS_MS, nullptr);
        StartSmoothness();
        SampleTick();
        ProcsTick();
        TrayUpdate();
        return 0;
    }
    case WM_SIZE:
        Layout(LOWORD(lp), HIWORD(lp));
        if (wp == SIZE_MINIMIZED && g_sv.Cfg().trayMode == 1) ShowWindow(hwnd, SW_HIDE);
        TrayUpdate();
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mi = (MINMAXINFO*)lp;
        mi->ptMinTrackSize.x = 960;
        mi->ptMinTrackSize.y = 640;
        return 0;
    }
    case WM_INITMENUPOPUP: {
        HMENU h = (HMENU)wp;
        if (h == g_menuPlansMain || h == g_menuPlansOpt) PopulatePlans(h);
        if (h == g_menuProfiles) PopulateProfiles(h);
        UpdatePopup(h);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        int code = HIWORD(wp);
        if (code == BN_CLICKED || code == 0) DoCommand(id);
        else if (code == EN_CHANGE && id == IDC_FILTER) FillProcList();
        else if (code == EN_CHANGE && id == IDC_LOGFILTER) RefreshLog();
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR* nh = (NMHDR*)lp;
        if (nh->code == TCN_SELCHANGE && nh->idFrom == IDC_VTAB) {
            g_activeOnly = TabCtrl_GetCurSel(g_vtab) == TAB_ACTIVE;
            FillProcList();
        } else if (nh->code == TCN_SELCHANGE && nh->idFrom == IDC_BTAB) {
            ShowBottom(TabCtrl_GetCurSel(g_btab));
        } else if (nh->code == NM_RCLICK && nh->idFrom == IDC_PROCLIST) {
            NMITEMACTIVATE* ia = (NMITEMACTIVATE*)lp;
            if (ia->iItem >= 0) {
                ListView_SetItemState(g_proc, ia->iItem, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                ShowProcMenu();
            }
        } else if (nh->code == NM_RCLICK && nh->idFrom == IDC_THREADLIST) {
            NMITEMACTIVATE* ia = (NMITEMACTIVATE*)lp;
            if (ia->iItem >= 0) {
                ListView_SetItemState(g_thList, ia->iItem, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                ShowThreadMenu();
            }
        } else if (nh->code == LVN_GETINFOTIPW && nh->idFrom == IDC_PROCLIST) {
            NMLVGETINFOTIPW* it = (NMLVGETINFOTIPW*)lp;
            LVITEMW li = {};
            li.mask = LVIF_PARAM;
            li.iItem = it->iItem;
            if (ListView_GetItem(g_proc, &li) && li.lParam >= 0 && li.lParam < (LPARAM)g_procs.size())
                wcsncpy_s(it->pszText, it->cchTextMax, g_procs[(size_t)li.lParam].path.c_str(), _TRUNCATE);
        }
        return 0;
    }
    case WM_TRAY:
        if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK) ShowMain();
        else if (lp == WM_RBUTTONUP) ShowTrayMenu();
        return 0;
    case WM_CLOSE:
        if (g_sv.Cfg().closeToTray) {
            ShowWindow(hwnd, SW_HIDE);
            TrayUpdate();
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN: {
        if (!g_sv.Cfg().darkTheme) break;
        HDC dc = (HDC)wp;
        SetTextColor(dc, RGB(230, 230, 230));
        SetBkColor(dc, RGB(32, 34, 36));
        return (LRESULT)g_darkBrush;
    }
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, g_sv.Cfg().darkTheme && g_darkBrush ? g_darkBrush : GetSysColorBrush(COLOR_BTNFACE));
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        PaintGraph(dc, rc.right);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_SAMPLE) SampleTick();
        else if (wp == TIMER_PROCS) ProcsTick();
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_SAMPLE);
        KillTimer(hwnd, TIMER_PROCS);
        StopSmoothness();
        if (g_trayAdded) { Shell_NotifyIconW(NIM_DELETE, &g_nid); g_trayAdded = false; }
        g_sv.SetKeepAwake(0);
        if (g_darkBrush) DeleteObject(g_darkBrush);
        DeleteObject(g_titleFont);
        DeleteObject(g_smallFont);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------- service mode ----------
static SERVICE_STATUS_HANDLE g_ssh;
static SERVICE_STATUS g_ss;
static HANDLE g_stopEvt;

static void WINAPI SvcCtrl(DWORD c) {
    if (c == SERVICE_CONTROL_STOP || c == SERVICE_CONTROL_SHUTDOWN) {
        g_ss.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(g_ssh, &g_ss);
        SetEvent(g_stopEvt);
    } else SetServiceStatus(g_ssh, &g_ss);
}
static void WINAPI SvcMain(DWORD, LPWSTR*) {
    g_ssh = RegisterServiceCtrlHandlerW(L"OpenLasso", SvcCtrl);
    if (!g_ssh) return;
    g_ss = {};
    g_ss.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ss.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    g_ss.dwCurrentState = SERVICE_RUNNING;
    g_stopEvt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetServiceStatus(g_ssh, &g_ss);
    ProcessSupervisor sv;
    sv.SetServiceMode(true);
    sv.AddLog(L"Service started");
    while (WaitForSingleObject(g_stopEvt, 1000) == WAIT_TIMEOUT) {
        sv.LoadConfig();
        sv.SampleSystemLoad();
        std::vector<ProcessInfo> procs = sv.EnumerateProcesses();
        if (!sv.EngineStopped()) sv.ApplyAll(procs);
        sv.ServiceHeartbeat();
        sv.TickKeepAwake();
    }
    sv.AddLog(L"Service stopped");
    CloseHandle(g_stopEvt);
    g_ss.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_ssh, &g_ss);
}

// ---------- entry point ----------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR cmd, int show) {
    std::wstring args = cmd ? cmd : L"";
    if (args.find(L"/service") != std::wstring::npos) {
        wchar_t name[] = L"OpenLasso";
        SERVICE_TABLE_ENTRYW tbl[] = { { name, SvcMain }, { nullptr, nullptr } };
        StartServiceCtrlDispatcherW(tbl);
        return 0;
    }
    { bool handled = false; int rc = RunCommandLine(args, handled); if (handled) return rc; }
    ProcessSupervisor cli;
    if (args.find(L"/install") != std::wstring::npos)   return cli.InstallService() ? 0 : 1;
    if (args.find(L"/uninstall") != std::wstring::npos) return cli.UninstallService() ? 0 : 1;
    if (args.find(L"/start") != std::wstring::npos)     return cli.StartSvc() ? 0 : 1;
    if (args.find(L"/stop") != std::wstring::npos)      return cli.StopSvc() ? 0 : 1;
    g_startMinimized = args.find(L"/minimized") != std::wstring::npos;

    // one instance only: a second launch brings the running window forward
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"OpenLasso_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"OpenLassoWnd", nullptr);
        if (other) { ShowWindow(other, SW_SHOW); ShowWindow(other, SW_RESTORE); SetForegroundWindow(other); }
        return 0;
    }

    Gdiplus::GdiplusStartupInput gi;
    ULONG_PTR gdiTok = 0;
    Gdiplus::GdiplusStartup(&gdiTok, &gi, nullptr);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_TAB_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpszClassName = L"OpenLassoWnd";
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(2));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);

    HWND hw = CreateWindowExW(0, wc.lpszClassName, L"OpenLasso",
                              WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1200, 760, nullptr, nullptr, hInst, nullptr);
    // First run: enable start with Windows (elevated, at sign-in) and remember it in the config.
    {
        ProcessSupervisor boot;
        if (GetFileAttributesW(boot.ConfigPath().c_str()) == INVALID_FILE_ATTRIBUTES) {
            wchar_t exe[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            ProcessSupervisor::SetAutostart(true, exe);
        }
    }
    // Start minimized to the tray (setting, default on, or the /minimized switch).
    bool hideAtStart = g_startMinimized;
    {
        ProcessSupervisor boot;
        boot.LoadConfig();
        if (boot.Cfg().startMinimized) hideAtStart = true;
    }
    ShowWindow(hw, hideAtStart ? SW_HIDE : show);
    UpdateWindow(hw);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hw, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    Gdiplus::GdiplusShutdown(gdiTok);
    if (mutex) CloseHandle(mutex);
    return (int)m.wParam;
}
