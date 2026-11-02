// process_supervisor.cpp - OpenLasso engine implementation.
#include "process_supervisor.h"
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <shellapi.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <functional>
#include <iterator>
#include <set>
#include <string>
#include <vector>

static const wchar_t* kServiceName = L"OpenLasso";
static const ULONGLONG kMaxLogBytes = 1024 * 1024;

const PriorityEntry kPriorities[] = {
    { IDLE_PRIORITY_CLASS,         L"IDLE_PRIORITY_CLASS",         L"Idle (low)" },
    { BELOW_NORMAL_PRIORITY_CLASS, L"BELOW_NORMAL_PRIORITY_CLASS", L"Below normal" },
    { NORMAL_PRIORITY_CLASS,       L"NORMAL_PRIORITY_CLASS",       L"Normal" },
    { ABOVE_NORMAL_PRIORITY_CLASS, L"ABOVE_NORMAL_PRIORITY_CLASS", L"Above normal" },
    { HIGH_PRIORITY_CLASS,         L"HIGH_PRIORITY_CLASS",         L"High" },
    { REALTIME_PRIORITY_CLASS,     L"REALTIME_PRIORITY_CLASS",     L"Realtime (risky)" },
};
const int kPriorityCount = (int)(sizeof(kPriorities) / sizeof(kPriorities[0]));
const wchar_t* kIoLabels[4] = { L"Very low", L"Low", L"Normal", L"High" };

std::wstring PriorityToLabel(DWORD value) {
    for (int i = 0; i < kPriorityCount; ++i)
        if (kPriorities[i].value == value) return kPriorities[i].label;
    return L"-";
}

// ---------- helpers ----------
static std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::string JsonEscape(const std::string& s) {
    std::string r;
    for (char c : s) {
        if (c == '\\' || c == '"') { r += '\\'; r += c; }
        else if (c == '\n') r += "\\n";
        else if (c == '\t') r += "\\t";
        else if (c == '\r') r += "\\r";
        else r += c;
    }
    return r;
}
static bool ExtractValue(const std::string& obj, const char* key, std::string& out) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = obj.find(k);
    if (p == std::string::npos) return false;
    p = obj.find(':', p + k.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < obj.size() && isspace((unsigned char)obj[p])) ++p;
    if (p >= obj.size()) return false;
    if (obj[p] == '"') {
        ++p;
        std::string r;
        while (p < obj.size() && obj[p] != '"') {
            if (obj[p] == '\\' && p + 1 < obj.size()) {
                ++p;
                char c = obj[p];
                r += (c == 'n') ? '\n' : (c == 't') ? '\t' : (c == 'r') ? '\r' : c;
            } else r += obj[p];
            ++p;
        }
        out = r;
        return true;
    }
    size_t e = p;
    while (e < obj.size() && obj[e] != ',' && obj[e] != '}' && obj[e] != ']' && !isspace((unsigned char)obj[e])) ++e;
    out = obj.substr(p, e - p);
    return true;
}
// Index of the bracket that closes the one at 'open'; strings and escapes are skipped (so values such as
// command lines may contain brackets). npos when unbalanced.
static size_t FindMatching(const std::string& s, size_t open, char oc, char cc) {
    int depth = 0;
    bool inStr = false;
    for (size_t i = open; i < s.size(); ++i) {
        char ch = s[i];
        if (inStr) {
            if (ch == '\\') ++i;
            else if (ch == '"') inStr = false;
            continue;
        }
        if (ch == '"') inStr = true;
        else if (ch == oc) ++depth;
        else if (ch == cc && --depth == 0) return i;
    }
    return std::string::npos;
}
// The flat {...} object that follows "key".
static bool Section(const std::string& c, const char* key, std::string& out) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = c.find(k);
    if (p == std::string::npos) return false;
    size_t ob = c.find('{', p);
    if (ob == std::string::npos) return false;
    size_t cb = FindMatching(c, ob, '{', '}');
    if (cb == std::string::npos) return false;
    out = c.substr(ob, cb - ob + 1);
    return true;
}
static bool StringArray(const std::string& c, const char* key, std::vector<std::string>& out) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = c.find(k);
    if (p == std::string::npos) return false;
    size_t a = c.find('[', p);
    size_t e = (a == std::string::npos) ? std::string::npos : FindMatching(c, a, '[', ']');
    if (e == std::string::npos) return false;
    out.clear();
    size_t i = a + 1;
    while (i < e) {
        size_t q1 = c.find('"', i);
        if (q1 == std::string::npos || q1 > e) break;
        size_t q2 = c.find('"', q1 + 1);
        if (q2 == std::string::npos || q2 > e) break;
        out.push_back(c.substr(q1 + 1, q2 - q1 - 1));
        i = q2 + 1;
    }
    return true;
}
static DWORD PriorityFromName(const std::wstring& n) {
    for (int i = 0; i < kPriorityCount; ++i)
        if (_wcsicmp(n.c_str(), kPriorities[i].cfgName) == 0) return kPriorities[i].value;
    return 0;
}
static const wchar_t* PriorityCfgName(DWORD v) {
    for (int i = 0; i < kPriorityCount; ++i)
        if (kPriorities[i].value == v) return kPriorities[i].cfgName;
    return L"";
}
static ULONGLONG ToU64(const FILETIME& f) {
    return ((ULONGLONG)f.dwHighDateTime << 32) | (ULONGLONG)f.dwLowDateTime;
}
static std::wstring GuidToText(const GUID& g) {
    wchar_t buf[64] = {};
    StringFromGUID2(g, buf, 64);
    return buf;
}
static bool GuidFromText(const std::wstring& t, GUID& g) {
    return CLSIDFromString(t.c_str(), &g) == S_OK;
}
static std::wstring LowerW(std::wstring s) {
    for (auto& ch : s) ch = (wchar_t)towlower(ch);
    return s;
}
static DWORD ForegroundPid() {
    HWND h = GetForegroundWindow();
    if (!h) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return pid;
}
static DWORD_PTR SystemAffinityMask() {
    DWORD_PTR pm = 0, sm = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm);
    return sm;
}
static DWORD IdleSeconds() {
    LASTINPUTINFO li{};
    li.cbSize = sizeof(li);
    if (!GetLastInputInfo(&li)) return 0;
    return (GetTickCount() - li.dwTime) / 1000;
}
static bool ReadWholeFile(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz = {};
    GetFileSizeEx(h, &sz);
    if (sz.QuadPart > 16LL * 1024 * 1024) { CloseHandle(h); return false; }
    out.assign((size_t)sz.QuadPart, '\0');
    DWORD got = 0;
    BOOL ok = TRUE;
    if (!out.empty()) ok = ReadFile(h, &out[0], (DWORD)out.size(), &got, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    out.resize(got);
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF)
        out.erase(0, 3);
    return true;
}
static bool WriteWholeFile(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr);
    CloseHandle(h);
    return ok && written == data.size();
}

// ---------- configuration text ----------
static void AddB(std::string& o, const char* k, bool v) { o += std::string("\"") + k + "\": " + (v ? "true" : "false") + ", "; }
static void AddI(std::string& o, const char* k, long long v) { o += std::string("\"") + k + "\": " + std::to_string(v) + ", "; }
static void AddS(std::string& o, const char* k, const std::wstring& v) {
    o += std::string("\"") + k + "\": \"" + JsonEscape(ToUtf8(v)) + "\", ";
}
static void PB(const std::string& s, const char* k, bool& out) {
    std::string v;
    if (ExtractValue(s, k, v)) out = (v == "true" || v == "1");
}
static void PI(const std::string& s, const char* k, int& out) {
    std::string v;
    if (ExtractValue(s, k, v)) out = atoi(v.c_str());
}
static void PU(const std::string& s, const char* k, DWORD& out) {
    std::string v;
    if (ExtractValue(s, k, v)) out = (DWORD)strtoul(v.c_str(), nullptr, 0);
}
static void PS(const std::string& s, const char* k, std::wstring& out) {
    std::string v;
    if (ExtractValue(s, k, v)) out = FromUtf8(v);
}

static void ParseRules(const std::string& c, std::vector<ProcessRule>& out) {
    out.clear();
    size_t p = c.find("\"rules\"");
    if (p == std::string::npos) return;
    size_t arr = c.find('[', p);
    size_t arrEnd = (arr == std::string::npos) ? std::string::npos : FindMatching(c, arr, '[', ']');
    if (arr == std::string::npos || arrEnd == std::string::npos) return;
    size_t pos = arr;
    while (true) {
        size_t ob = c.find('{', pos);
        if (ob == std::string::npos || ob > arrEnd) break;
        size_t oe = FindMatching(c, ob, '{', '}');
        if (oe == std::string::npos) break;
        std::string obj = c.substr(ob, oe - ob + 1);
        ProcessRule r;
        std::string s;
        if (ExtractValue(obj, "process", s)) r.processName = FromUtf8(s);
        if (ExtractValue(obj, "priority", s)) r.priorityClass = PriorityFromName(FromUtf8(s));
        if (ExtractValue(obj, "affinity", s)) r.affinityMask = (DWORD_PTR)strtoull(s.c_str(), nullptr, 0);
        if (ExtractValue(obj, "memory", s)) { int m = atoi(s.c_str()); r.memoryPriority = (m >= 1 && m <= 5) ? (DWORD)m : 0; }
        if (ExtractValue(obj, "efficiency", s)) r.eco = (s == "on" || s == "true") ? 1 : (s == "off" || s == "false") ? 2 : 0;
        if (ExtractValue(obj, "io", s)) { int v = atoi(s.c_str()); r.ioPriority = (v >= 1 && v <= 4) ? (DWORD)v : 0; }
        if (ExtractValue(obj, "thread_boost", s)) r.threadBoost = (s == "on" || s == "true") ? 1 : (s == "off" || s == "false") ? 2 : 0;
        if (ExtractValue(obj, "cpu_sets", s)) r.cpuSets = std::clamp(atoi(s.c_str()), 0, 3);
        if (ExtractValue(obj, "gpu_priority", s)) r.gpuPriority = std::clamp(atoi(s.c_str()), 0, 6);
        if (ExtractValue(obj, "gpu_pref", s)) r.gpuPref = std::clamp(atoi(s.c_str()), 0, 3);
        if (ExtractValue(obj, "prevent_sleep", s)) r.preventSleep = (s == "true" || s == "1");
        if (ExtractValue(obj, "force_power", s)) r.forcePower = (s == "true" || s == "1");
        if (ExtractValue(obj, "limit_percent", s)) r.limitPercent = std::clamp(atoi(s.c_str()), 0, 100);
        if (ExtractValue(obj, "guard_cpu", s)) r.guardCpu = std::clamp(atoi(s.c_str()), 0, 100);
        if (ExtractValue(obj, "guard_seconds", s)) r.guardSeconds = std::clamp(atoi(s.c_str()), 0, 86400);
        if (ExtractValue(obj, "guard_action", s)) r.guardAction = std::clamp(atoi(s.c_str()), 0, 3);
        if (ExtractValue(obj, "guard_command", s)) r.guardCommand = FromUtf8(s);
        if (ExtractValue(obj, "block", s)) r.block = (s == "true" || s == "1");
        if (ExtractValue(obj, "balance_instances", s)) r.balanceInstances = (s == "true" || s == "1");
        if (ExtractValue(obj, "power_plan", s)) r.powerPlan = FromUtf8(s);
        if (ExtractValue(obj, "keep_running", s)) r.keepRunning = (s == "true" || s == "1");
        if (ExtractValue(obj, "keep_running_elevate", s)) r.keepRunningElevate = (s == "true" || s == "1");
        if (ExtractValue(obj, "keep_running_cmd", s)) r.keepRunningCommandLine = FromUtf8(s);
        if (ExtractValue(obj, "keep_running_path", s)) r.keepRunningPath = FromUtf8(s);
        if (ExtractValue(obj, "enabled", s)) r.enabled = (s == "true" || s == "1");
        if (!r.processName.empty()) out.push_back(r);
        pos = oe + 1;
    }
}

static void ParseConfigText(const std::string& c, std::vector<ProcessRule>& rules, Settings& cfg) {
    std::string s;
    if (Section(c, "settings", s)) {
        PB(s, "throttle", cfg.throttle);
        PI(s, "load_threshold", cfg.loadThreshold);
        PI(s, "process_threshold", cfg.procThreshold);
        PI(s, "throttle_seconds", cfg.throttleSeconds);
        PB(s, "notifications", cfg.notifications);
        PB(s, "delegate_to_service", cfg.delegateToService);
        PB(s, "boost", cfg.boost);
        PB(s, "auto_apply", cfg.autoApply);
        PB(s, "forced", cfg.forced);
        PB(s, "memory_optimizer", cfg.memoryOptimizer);
        PB(s, "idle_saver", cfg.idleSaver);
        PI(s, "idle_minutes", cfg.idleMinutes);
        PB(s, "performance", cfg.performance);
        PB(s, "manage_all_users", cfg.manageAllUsers);
        PB(s, "close_to_tray", cfg.closeToTray);
        PB(s, "start_minimized", cfg.startMinimized);
        PI(s, "tray_mode", cfg.trayMode);
        PB(s, "log_enabled", cfg.logEnabled);
        PB(s, "smooth", cfg.smooth);
        PB(s, "show_events", cfg.showEvents);
        PB(s, "show_cpu_meter", cfg.showCpuMeter);
        PB(s, "show_ram_meter", cfg.showRamMeter);
        PB(s, "show_plan", cfg.showPlan);
        PB(s, "show_cpu_line", cfg.showCpuLine);
        PB(s, "tree_view", cfg.treeView);
        PB(s, "gridlines", cfg.gridlines);
        PB(s, "icons", cfg.icons);
        PB(s, "tooltips", cfg.tooltips);
        PB(s, "always_on_top", cfg.alwaysOnTop);
        PB(s, "dark_theme", cfg.darkTheme);
        PB(s, "cpu_utility", cfg.cpuUtility);
        PB(s, "pause_on_right_click", cfg.pauseOnRightClick);
        PB(s, "show_ram_line", cfg.showRamLine);
        PB(s, "show_gpu_line", cfg.showGpuLine);
        PB(s, "show_gpu_meter", cfg.showGpuMeter);
        PB(s, "gpu_fallback", cfg.gpuFallback);
        PB(s, "custom_perf_plan", cfg.customPerfPlan);
        PI(s, "max_log_kb", cfg.maxLogKB);
        cfg.maxLogKB = std::clamp(cfg.maxLogKB, 64, 65536);
        PU(s, "columns_mask", cfg.columnsMask);
        int fmt = 0;
        PI(s, "format", fmt);
        if (fmt < 3) cfg.columnsMask |= 0xA000u;      // older files: show the new GPU % and RAM columns
        PI(s, "update_check", cfg.updateCheck);
        PB(s, "include_betas", cfg.includeBetas);
        PS(s, "update_repo", cfg.updateRepo);
        PS(s, "log_folder", cfg.logFolder);
    }
    std::vector<std::string> ex;
    if (StringArray(c, "exclusions", ex)) {
        cfg.exclusions.clear();
        for (const std::string& e : ex) if (!e.empty()) cfg.exclusions.push_back(FromUtf8(e));
    }
    ParseRules(c, rules);
}

static std::string RulesArrayText(const std::vector<ProcessRule>& rules) {
    std::string o = "[\n";
    for (size_t i = 0; i < rules.size(); ++i) {
        const ProcessRule& r = rules[i];
        char aff[32];
        snprintf(aff, sizeof(aff), "0x%llX", (unsigned long long)r.affinityMask);
        const char* eff = (r.eco == 1) ? "on" : (r.eco == 2) ? "off" : "";
        const char* tb = (r.threadBoost == 1) ? "on" : (r.threadBoost == 2) ? "off" : "";
        o += "    { \"process\": \"" + JsonEscape(ToUtf8(r.processName)) + "\", ";
        o += "\"priority\": \"" + ToUtf8(PriorityCfgName(r.priorityClass)) + "\", ";
        o += "\"affinity\": \"" + std::string(aff) + "\", ";
        o += "\"memory\": " + std::to_string(r.memoryPriority) + ", ";
        o += "\"efficiency\": \"" + std::string(eff) + "\", ";
        o += "\"io\": " + std::to_string(r.ioPriority) + ", ";
        o += "\"thread_boost\": \"" + std::string(tb) + "\", ";
        o += "\"cpu_sets\": " + std::to_string(r.cpuSets) + ", ";
        o += "\"gpu_priority\": " + std::to_string(r.gpuPriority) + ", ";
        o += "\"gpu_pref\": " + std::to_string(r.gpuPref) + ", ";
        o += std::string("\"prevent_sleep\": ") + (r.preventSleep ? "true" : "false") + ", ";
        o += std::string("\"force_power\": ") + (r.forcePower ? "true" : "false") + ", ";
        o += "\"limit_percent\": " + std::to_string(r.limitPercent) + ", ";
        o += "\"guard_cpu\": " + std::to_string(r.guardCpu) + ", ";
        o += "\"guard_seconds\": " + std::to_string(r.guardSeconds) + ", ";
        o += "\"guard_action\": " + std::to_string(r.guardAction) + ", ";
        o += "\"guard_command\": \"" + JsonEscape(ToUtf8(r.guardCommand)) + "\", ";
        o += std::string("\"block\": ") + (r.block ? "true" : "false") + ", ";
        o += std::string("\"balance_instances\": ") + (r.balanceInstances ? "true" : "false") + ", ";
        o += "\"power_plan\": \"" + JsonEscape(ToUtf8(r.powerPlan)) + "\", ";
        o += std::string("\"keep_running\": ") + (r.keepRunning ? "true" : "false") + ", ";
        o += std::string("\"keep_running_elevate\": ") + (r.keepRunningElevate ? "true" : "false") + ", ";
        o += "\"keep_running_cmd\": \"" + JsonEscape(ToUtf8(r.keepRunningCommandLine)) + "\", ";
        o += "\"keep_running_path\": \"" + JsonEscape(ToUtf8(r.keepRunningPath)) + "\", ";
        o += std::string("\"enabled\": ") + (r.enabled ? "true" : "false") + " }";
        o += (i + 1 < rules.size()) ? ",\n" : "\n";
    }
    o += "  ]";
    return o;
}

static std::string BuildConfigText(const std::vector<ProcessRule>& rules, const Settings& c) {
    std::string o = "{\n  \"application\": \"OpenLasso\",\n  \"settings\": { ";
    AddB(o, "throttle", c.throttle);
    AddI(o, "load_threshold", c.loadThreshold);
    AddI(o, "process_threshold", c.procThreshold);
    AddI(o, "throttle_seconds", c.throttleSeconds);
    AddB(o, "notifications", c.notifications);
    AddB(o, "delegate_to_service", c.delegateToService);
    AddB(o, "boost", c.boost);
    AddB(o, "auto_apply", c.autoApply);
    AddB(o, "forced", c.forced);
    AddB(o, "memory_optimizer", c.memoryOptimizer);
    AddB(o, "idle_saver", c.idleSaver);
    AddI(o, "idle_minutes", c.idleMinutes);
    AddB(o, "performance", c.performance);
    AddB(o, "manage_all_users", c.manageAllUsers);
    AddB(o, "close_to_tray", c.closeToTray);
    AddB(o, "start_minimized", c.startMinimized);
    AddI(o, "tray_mode", c.trayMode);
    AddB(o, "log_enabled", c.logEnabled);
    AddB(o, "smooth", c.smooth);
    AddB(o, "show_events", c.showEvents);
    AddB(o, "show_cpu_meter", c.showCpuMeter);
    AddB(o, "show_ram_meter", c.showRamMeter);
    AddB(o, "show_plan", c.showPlan);
    AddB(o, "show_cpu_line", c.showCpuLine);
    AddB(o, "tree_view", c.treeView);
    AddB(o, "gridlines", c.gridlines);
    AddB(o, "icons", c.icons);
    AddB(o, "tooltips", c.tooltips);
    AddB(o, "always_on_top", c.alwaysOnTop);
    AddB(o, "dark_theme", c.darkTheme);
    AddB(o, "cpu_utility", c.cpuUtility);
    AddB(o, "pause_on_right_click", c.pauseOnRightClick);
    AddB(o, "show_ram_line", c.showRamLine);
    AddB(o, "show_gpu_line", c.showGpuLine);
    AddB(o, "show_gpu_meter", c.showGpuMeter);
    AddB(o, "gpu_fallback", c.gpuFallback);
    AddB(o, "custom_perf_plan", c.customPerfPlan);
    AddI(o, "max_log_kb", c.maxLogKB);
    AddI(o, "columns_mask", (long long)c.columnsMask);
    AddI(o, "update_check", c.updateCheck);
    AddB(o, "include_betas", c.includeBetas);
    AddS(o, "update_repo", c.updateRepo);
    AddS(o, "log_folder", c.logFolder);
    o += "\"format\": 3 },\n  \"exclusions\": [";
    for (size_t i = 0; i < c.exclusions.size(); ++i) {
        o += "\"" + JsonEscape(ToUtf8(c.exclusions[i])) + "\"";
        if (i + 1 < c.exclusions.size()) o += ", ";
    }
    o += "],\n  \"rules\": " + RulesArrayText(rules) + "\n}\n";
    return o;
}

static void EnablePrivilege(const wchar_t* name) {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid))
        AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    CloseHandle(tok);
}
static std::wstring ComputerNameW() {
    static std::wstring name = []() {
        wchar_t b[MAX_COMPUTERNAME_LENGTH + 1] = {};
        DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
        return GetComputerNameW(b, &n) ? std::wstring(b) : std::wstring();
    }();
    return name;
}
static std::wstring CsvField(const std::wstring& s) {
    if (s.find_first_of(L",\"\r\n") == std::wstring::npos) return s;
    std::wstring r = L"\"";
    for (wchar_t ch : s) {
        if (ch == L'"') r += L"\"\"";
        else r += ch;
    }
    r += L'"';
    return r;
}
// Full command line of a process (NtQueryInformationProcess class 60, Windows 8.1+).
static std::wstring QueryCommandLine(HANDLE h) {
    typedef LONG(NTAPI * PFN)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static PFN fn = (PFN)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
    if (!fn) return L"";
    ULONG len = 0;
    fn(h, 60, nullptr, 0, &len);
    if (!len || len > 1024 * 1024) return L"";
    std::vector<BYTE> buf(len + 16);
    if (fn(h, 60, buf.data(), (ULONG)buf.size(), &len) != 0) return L"";
    struct UStr { USHORT Length, MaximumLength; PWSTR Buffer; };
    UStr* u = (UStr*)buf.data();
    if (!u->Buffer || !u->Length) return L"";
    return std::wstring(u->Buffer, u->Length / sizeof(wchar_t));
}

// ---------- ProcessSupervisor ----------
ProcessSupervisor::~ProcessSupervisor() { ResumeLimited(); ClosePdh(); CloseIoPdh(); }

ProcessSupervisor::ProcessSupervisor() {
    EnablePrivilege(SE_DEBUG_NAME);               // reach more processes
    EnablePrivilege(SE_INC_BASE_PRIORITY_NAME);   // needed for Realtime (CPU and GPU)
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p = path;
    size_t slash = p.find_last_of(L"\\/");
    m_configPath = (slash == std::wstring::npos ? L"" : p.substr(0, slash + 1)) + L"OpenLasso.json";
}

std::wstring ProcessSupervisor::ExeDir() const {
    return m_configPath.substr(0, m_configPath.find_last_of(L"\\/") + 1);
}

// ----- CPU and memory sampling (cheap; the GUI calls it every 250 ms) -----
void ProcessSupervisor::SampleSystemLoad() {
    FILETIME idle{}, kern{}, user{};
    if (GetSystemTimes(&idle, &kern, &user)) {
        ULONGLONG i = ToU64(idle), k = ToU64(kern), u = ToU64(user);
        if (m_sysValid) {
            ULONGLONG di = i - m_prevIdle, dk = k - m_prevKern, du = u - m_prevUser;
            ULONGLONG total = dk + du;
            if (total > 0 && di <= total) m_systemLoad = 100.0 * (double)(total - di) / (double)total;
        }
        m_prevIdle = i; m_prevKern = k; m_prevUser = u; m_sysValid = true;
    }
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        m_memLoad = ms.dwMemoryLoad;
        m_memTotalGB = ms.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
        m_memUsedGB = (ms.ullTotalPhys - ms.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0);
    }
    SampleExtras();
}

static std::wstring UserOfProcess(HANDLE h) {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(h, TOKEN_QUERY, &tok)) return L"";
    std::wstring result;
    DWORD need = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &need);
    if (need) {
        std::vector<BYTE> buf(need);
        if (GetTokenInformation(tok, TokenUser, buf.data(), need, &need)) {
            PSID sid = ((TOKEN_USER*)buf.data())->User.Sid;
            wchar_t name[256] = {}, dom[256] = {};
            DWORD nl = 256, dl = 256;
            SID_NAME_USE use;
            if (LookupAccountSidW(nullptr, sid, name, &nl, dom, &dl, &use)) result = name;
        }
    }
    CloseHandle(tok);
    return result;
}

std::vector<ProcessInfo> ProcessSupervisor::EnumerateProcesses() {
    ULONGLONG now = GetTickCount64();
    double elapsedMs = (double)(now - m_prevTick);
    bool computeNow = (m_prevTick == 0) || elapsedMs >= 500.0;
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    double cores = si.dwNumberOfProcessors ? (double)si.dwNumberOfProcessors : 1.0;
    std::wstring me = L"";
    std::vector<ProcessInfo> list;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return list;

    std::map<DWORD, ULONGLONG> times;
    std::map<DWORD, std::wstring> users;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            ProcessInfo pi;
            pi.pid = pe.th32ProcessID;
            pi.parentPid = pe.th32ParentProcessID;
            pi.threads = pe.cntThreads;
            pi.name = pe.szExeFile;
            {
                auto gp = m_gpuPid.find(pi.pid);
                if (gp != m_gpuPid.end()) pi.gpuPercent = gp->second;
                auto gm = m_gpuMem.find(pi.pid);
                if (gm != m_gpuMem.end()) pi.gpuMemBytes = gm->second;
            }
            ULONGLONG cpuTime = 0;
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pi.pid);
            if (h) {
                pi.priorityClass = ::GetPriorityClass(h);
                DWORD_PTR pm = 0, sm = 0;
                if (GetProcessAffinityMask(h, &pm, &sm)) pi.affinityMask = pm;
                MEMORY_PRIORITY_INFORMATION mi{};
                if (GetProcessInformation(h, ProcessMemoryPriority, &mi, sizeof(mi))) pi.memoryPriority = mi.MemoryPriority;
                PROCESS_POWER_THROTTLING_STATE pts{};
                pts.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
                if (GetProcessInformation(h, ProcessPowerThrottling, &pts, sizeof(pts)))
                    pi.ecoQos = (pts.ControlMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) &&
                                (pts.StateMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED);
                FILETIME c{}, e{}, kt{}, ut{};
                if (GetProcessTimes(h, &c, &e, &kt, &ut)) cpuTime = ToU64(kt) + ToU64(ut);
                pi.cpuTimeMs = cpuTime / 10000;
                auto cached = m_userCache.find(pi.pid);
                pi.user = (cached != m_userCache.end()) ? cached->second : UserOfProcess(h);
                users[pi.pid] = pi.user;
                wchar_t pathBuf[1024] = {};
                DWORD psz = 1024;
                if (QueryFullProcessImageNameW(h, 0, pathBuf, &psz)) pi.path = pathBuf;
                PROCESS_MEMORY_COUNTERS pmc{};
                if (GetProcessMemoryInfo(h, &pmc, sizeof(pmc))) pi.workingSetKB = (ULONGLONG)(pmc.WorkingSetSize / 1024);
                CloseHandle(h);
            }
            // I/O priority needs a query-capable handle
            HANDLE hq = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pi.pid);
            if (hq) {
                static auto qf = (LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG))
                    GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
                ULONG io = 0;
                if (qf && qf(hq, 33, &io, sizeof(io), nullptr) == 0) pi.ioPriority = io;
                else pi.ioPriority = 99;
                CloseHandle(hq);
            } else pi.ioPriority = 99;

            times[pi.pid] = cpuTime;
            if (computeNow) {
                if (m_prevTick != 0) {
                    auto it = m_prevTimes.find(pi.pid);
                    if (it != m_prevTimes.end() && cpuTime >= it->second) {
                        double ms = (double)(cpuTime - it->second) / 10000.0;
                        pi.cpuPercent = 100.0 * ms / (elapsedMs * cores);
                    }
                }
                m_lastCpu[pi.pid] = pi.cpuPercent;
            } else {
                auto it = m_lastCpu.find(pi.pid);
                if (it != m_lastCpu.end()) pi.cpuPercent = it->second;
            }
            list.push_back(pi);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    if (computeNow) {
        m_prevTimes = times;
        m_prevTick = now;
        for (auto it = m_lastCpu.begin(); it != m_lastCpu.end();)
            it = times.count(it->first) ? std::next(it) : m_lastCpu.erase(it);
    }
    m_userCache = users;
    for (auto it = m_cmdCache.begin(); it != m_cmdCache.end();)
        it = times.count(it->first) ? std::next(it) : m_cmdCache.erase(it);
    if (!m_cfg.manageAllUsers) {
        std::wstring mine;
        for (const auto& p : list) if (p.pid == GetCurrentProcessId()) mine = p.user;
        list.erase(std::remove_if(list.begin(), list.end(), [&](const ProcessInfo& p) { return p.user != mine; }), list.end());
    }
    return list;
}

// ----- per-process actions -----
bool ProcessSupervisor::SetPriorityClass(DWORD pid, DWORD priority) {
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
    if (!h) return false;
    BOOL ok = ::SetPriorityClass(h, priority);
    CloseHandle(h);
    return ok != FALSE;
}
bool ProcessSupervisor::SetAffinityMask(DWORD pid, DWORD_PTR mask) {
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    DWORD_PTR pm = 0, sm = 0;
    bool ok = false;
    if (GetProcessAffinityMask(h, &pm, &sm)) {
        mask &= sm;
        if (mask) ok = ::SetProcessAffinityMask(h, mask) != FALSE;
    }
    CloseHandle(h);
    return ok;
}
bool ProcessSupervisor::SetMemoryPriority(DWORD pid, DWORD prio) {
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
    if (!h) return false;
    MEMORY_PRIORITY_INFORMATION mi{};
    mi.MemoryPriority = prio;
    BOOL ok = SetProcessInformation(h, ProcessMemoryPriority, &mi, sizeof(mi));
    CloseHandle(h);
    return ok != FALSE;
}
bool ProcessSupervisor::SetEcoQos(DWORD pid, bool on) {
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    PROCESS_POWER_THROTTLING_STATE pts{};
    pts.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    pts.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    pts.StateMask = on ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0;
    BOOL ok = SetProcessInformation(h, ProcessPowerThrottling, &pts, sizeof(pts));
    CloseHandle(h);
    return ok != FALSE;
}
// I/O priority uses NtSetInformationProcess(ProcessIoPriority = 33), resolved at run time from ntdll.
bool ProcessSupervisor::SetIoPriority(DWORD pid, DWORD prio) {
    typedef LONG(NTAPI * PFN)(HANDLE, ULONG, PVOID, ULONG);
    static PFN fn = (PFN)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationProcess");
    if (!fn || prio > 3) return false;
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
    if (!h) return false;
    ULONG v = (ULONG)prio;
    LONG st = fn(h, 33, &v, sizeof(v));
    CloseHandle(h);
    return st == 0;
}
bool ProcessSupervisor::SetThreadBoost(DWORD pid, bool on) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    int ok = 0;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            HANDLE th = OpenThread(THREAD_SET_INFORMATION, FALSE, te.th32ThreadID);
            if (th) {
                if (SetThreadPriorityBoost(th, !on)) ++ok;
                CloseHandle(th);
            }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return ok > 0;
}
bool ProcessSupervisor::FreeProcessMemory(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) return false;
    BOOL ok = EmptyWorkingSet(h);
    CloseHandle(h);
    return ok != FALSE;
}
struct CloseCtx { DWORD pid; int count; };
static BOOL CALLBACK CloseEnum(HWND hw, LPARAM lp) {
    CloseCtx* c = (CloseCtx*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(hw, &pid);
    if (pid == c->pid && IsWindowVisible(hw) && GetWindow(hw, GW_OWNER) == nullptr) {
        PostMessageW(hw, WM_CLOSE, 0, 0);
        ++c->count;
    }
    return TRUE;
}
bool ProcessSupervisor::CloseProcessWindows(DWORD pid) {
    CloseCtx c{ pid, 0 };
    EnumWindows(CloseEnum, (LPARAM)&c);
    return c.count > 0;
}
bool ProcessSupervisor::EndProcess(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!h) return false;
    BOOL ok = ::TerminateProcess(h, 1);
    CloseHandle(h);
    return ok != FALSE;
}
std::wstring ProcessSupervisor::CommandLineOf(DWORD pid) {
    auto it = m_cmdCache.find(pid);
    if (it != m_cmdCache.end()) return it->second;
    std::wstring r;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) {
        r = QueryCommandLine(h);
        CloseHandle(h);
    }
    if (!r.empty()) m_cmdCache[pid] = r;
    return r;
}
// "C:\\x y\\app.exe" -a -b  ->  -a -b
std::wstring ProcessSupervisor::StripArgv0(const std::wstring& cmd) {
    size_t i = 0;
    while (i < cmd.size() && iswspace(cmd[i])) ++i;
    if (i < cmd.size() && cmd[i] == L'"') {
        size_t e = cmd.find(L'"', i + 1);
        if (e == std::wstring::npos) return L"";
        i = e + 1;
    } else {
        while (i < cmd.size() && !iswspace(cmd[i])) ++i;
    }
    while (i < cmd.size() && iswspace(cmd[i])) ++i;
    return cmd.substr(i);
}
bool ProcessSupervisor::LaunchProcess(const std::wstring& path, const std::wstring& args, bool elevate) {
    if (path.empty()) return false;
    size_t slash = path.find_last_of(L"\\/");
    std::wstring dir = (slash == std::wstring::npos) ? std::wstring() : path.substr(0, slash);
    if (elevate) {
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.lpVerb = L"runas";
        sei.lpFile = path.c_str();
        sei.lpParameters = args.empty() ? nullptr : args.c_str();
        sei.lpDirectory = dir.empty() ? nullptr : dir.c_str();
        sei.nShow = SW_SHOWNORMAL;
        return ShellExecuteExW(&sei) != FALSE;
    }
    std::wstring cmd = L"\"" + path + L"\"" + (args.empty() ? std::wstring() : L" " + args);
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(path.c_str(), buf.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        dir.empty() ? nullptr : dir.c_str(), &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}
bool ProcessSupervisor::RestartProcess(const ProcessInfo& p, bool asAdmin) {
    if (p.path.empty()) return false;
    // The command line is read BEFORE the process is ended: afterwards it can no longer be queried.
    std::wstring args = StripArgv0(p.cmdLine.empty() ? CommandLineOf(p.pid) : p.cmdLine);
    // Runs on the GUI thread: no engine tick can see the gap, so "keep running" does not start a second copy.
    if (!EndProcess(p.pid)) return false;
    Sleep(300);
    return LaunchProcess(p.path, args, asAdmin);
}
void ProcessSupervisor::NoteUserExit(const std::wstring& name) {
    auto it = m_keep.find(LowerW(name));
    if (it != m_keep.end()) it->second.userEnded = true;
}
bool ProcessSupervisor::ApplyToProcess(DWORD pid, const ProcessRule& r, const std::wstring& path,
                                       const ProcessInfo* info) {
    bool ok = true;
    m_lastNote.clear();
    if (r.cpuSets) ok = SetCpuSets(pid, r.cpuSets) && ok;
    if (r.gpuPriority) {
        ProcessInfo tmp;
        if (info) tmp = *info;
        else { tmp.pid = pid; tmp.name = L"PID " + std::to_wstring(pid); }
        if (tmp.path.empty()) tmp.path = path;
        m_gpuLogged.erase(pid);                       // a manual change is always reported
        GpuResult gr = ApplyGpuRule(tmp, r);
        if (gr == GpuResult::Denied || gr == GpuResult::Failed) ok = false;
    }
    if (r.gpuPref && !path.empty()) ok = SetGpuPreference(path, r.gpuPref - 1) && ok;
    if (r.priorityClass) ok = SetPriorityClass(pid, r.priorityClass) && ok;
    if (r.affinityMask) ok = SetAffinityMask(pid, r.affinityMask) && ok;
    if (r.memoryPriority) ok = SetMemoryPriority(pid, r.memoryPriority) && ok;
    if (r.eco) ok = SetEcoQos(pid, r.eco == 1) && ok;
    if (r.ioPriority) ok = SetIoPriority(pid, r.ioPriority - 1) && ok;
    if (r.threadBoost) ok = SetThreadBoost(pid, r.threadBoost == 1) && ok;
    return ok;
}

bool ProcessSupervisor::IsRuleControlled(const std::wstring& name) const {
    for (const ProcessRule& r : m_rules)
        if (r.enabled && r.processName != L"*" && MatchGlob(r.processName, name)) return true;
    return false;
}
bool ProcessSupervisor::IsExcluded(const std::wstring& name) const {
    for (const std::wstring& e : m_cfg.exclusions)
        if (_wcsicmp(e.c_str(), name.c_str()) == 0) return true;
    return false;
}

// ----- engine -----
int ProcessSupervisor::ApplyAll(const std::vector<ProcessInfo>& procs) {
    if (m_stopped) return 0;
    DWORD fg = ForegroundPid();
    int n = RunRules(procs);
    n += RunThrottle(procs, fg);
    n += RunBoost(procs, fg);
    n += RunMemoryOptimizer(procs, fg);
    n += RunFeatures(procs);
    n += RunHolds(procs);
    n += RunPowerPlan(procs);
    n += RunKeepRunning(procs);
    return n;
}

int ProcessSupervisor::RunRules(const std::vector<ProcessInfo>& procs) {
    int changed = 0;
    DWORD self = GetCurrentProcessId();
    DWORD_PTR systemMask = SystemAffinityMask();
    for (const ProcessRule& r : m_rules) {
        if (!r.enabled || r.processName.empty()) continue;
        for (const ProcessInfo& p : procs) {
            if (p.pid == 0 || p.pid == 4 || p.pid == self) continue;
            if (!RuleMatchesProcess(r, p, m_rules)) continue;
            bool forced = m_cfg.forced;
            if (r.priorityClass && (forced || p.priorityClass != r.priorityClass) && SetPriorityClass(p.pid, r.priorityClass)) {
                m_lowered.erase(p.pid);
                m_boosted.erase(p.pid);
                if (p.priorityClass != r.priorityClass) { ++changed; AddLog(L"Priority " + PriorityToLabel(r.priorityClass) + L" set for " + p.name, &p); }
            }
            DWORD_PTR want = r.affinityMask & systemMask;
            if (want && p.affinityMask && (forced || p.affinityMask != want) && SetAffinityMask(p.pid, r.affinityMask)) {
                if (p.affinityMask != want) { ++changed; AddLog(L"CPU affinity changed for " + p.name, &p); }
            }
            if (r.memoryPriority && (forced || p.memoryPriority != r.memoryPriority) && SetMemoryPriority(p.pid, r.memoryPriority)) {
                if (p.memoryPriority != r.memoryPriority) { ++changed; AddLog(L"Memory priority changed for " + p.name, &p); }
            }
            if (r.eco && (forced || p.ecoQos != (r.eco == 1)) && SetEcoQos(p.pid, r.eco == 1)) {
                if (p.ecoQos != (r.eco == 1)) { ++changed; AddLog(std::wstring(r.eco == 1 ? L"Efficiency mode on for " : L"Efficiency mode off for ") + p.name, &p); }
            }
            if (r.ioPriority && p.ioPriority != r.ioPriority - 1 && SetIoPriority(p.pid, r.ioPriority - 1)) {
                ++changed; AddLog(L"I/O priority set for " + p.name, &p);
            }
            if (r.threadBoost && (forced || m_appliedBoost[p.pid] != r.threadBoost))
                if (SetThreadBoost(p.pid, r.threadBoost == 1)) m_appliedBoost[p.pid] = r.threadBoost;
            if (r.cpuSets && (forced || m_appliedCpu[p.pid] != r.cpuSets))
                if (SetCpuSets(p.pid, r.cpuSets)) m_appliedCpu[p.pid] = r.cpuSets;
            if (r.gpuPriority && (forced || m_appliedGpu[p.pid] != r.gpuPriority)) {
                ApplyGpuRule(p, r);
                m_appliedGpu[p.pid] = r.gpuPriority;   // tried once; no retry every second if the driver refuses
            }
            if (r.gpuPref && !p.path.empty() && (forced || m_appliedPref[p.pid] != r.gpuPref))
                if (SetGpuPreference(p.path, r.gpuPref - 1)) m_appliedPref[p.pid] = r.gpuPref;
        }
    }
    return changed;
}

// Auto-throttle: under high total load, background processes that use much CPU are lowered
// to "Below normal" and restored when the load drops (10-point hysteresis).
int ProcessSupervisor::RunThrottle(const std::vector<ProcessInfo>& procs, DWORD fg) {
    int changed = 0;
    DWORD self = GetCurrentProcessId();
    std::map<DWORD, const ProcessInfo*> byPid;
    for (const ProcessInfo& p : procs) byPid[p.pid] = &p;
    for (auto it = m_lowered.begin(); it != m_lowered.end();)
        it = byPid.count(it->first) ? std::next(it) : m_lowered.erase(it);

    bool enabled = m_cfg.throttle;
    bool calm = !enabled || m_systemLoad < m_cfg.loadThreshold - 10;
    bool busy = enabled && m_systemLoad >= m_cfg.loadThreshold;

    if (calm || m_lowered.count(fg)) {
        for (auto it = m_lowered.begin(); it != m_lowered.end();) {
            bool restoreThis = calm || it->first == fg;
            if (!restoreThis) { ++it; continue; }
            auto f = byPid.find(it->first);
            if (f != byPid.end()) {
                const ProcessInfo* p = f->second;
                if (p->priorityClass == BELOW_NORMAL_PRIORITY_CLASS && !IsRuleControlled(p->name) &&
                    SetPriorityClass(p->pid, it->second)) {
                    ++changed;
                    AddLog(L"Auto-throttle: restored " + p->name, p);
                }
            }
            it = m_lowered.erase(it);
        }
    }
    if (!busy) return changed;

    for (const ProcessInfo& p : procs) {
        if (p.pid <= 4 || p.pid == self || p.pid == fg) continue;
        if (m_lowered.count(p.pid)) continue;
        if (p.cpuPercent < m_cfg.procThreshold) { m_hot[p.pid] = 0; continue; }
        if (++m_hot[p.pid] < m_cfg.throttleSeconds) continue;
        if (p.priorityClass != NORMAL_PRIORITY_CLASS && p.priorityClass != ABOVE_NORMAL_PRIORITY_CLASS) continue;
        if (IsExcluded(p.name) || IsRuleControlled(p.name)) continue;
        if (SetPriorityClass(p.pid, BELOW_NORMAL_PRIORITY_CLASS)) {
            m_lowered[p.pid] = p.priorityClass;
            m_lowerStart[p.pid] = GetTickCount64();
            CountEvent();
            RecordStat(p.name, p.cpuPercent);
            ++changed;
            AddLog(L"Auto-throttle: lowered " + p.name + L" (" + std::to_wstring((int)p.cpuPercent) + L"% CPU)", &p);
        }
    }
    return changed;
}

void ProcessSupervisor::CountEvent() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    if (st.wDay != m_pbDay) { m_pbDay = st.wDay; m_pbToday = 0; }
    ++m_pbToday;
    ++m_pbTotal;
}

// Active window boost: the active window gets "Above normal" while it has focus.
int ProcessSupervisor::RunBoost(const std::vector<ProcessInfo>& procs, DWORD fg) {
    int changed = 0;
    std::map<DWORD, const ProcessInfo*> byPid;
    for (const ProcessInfo& p : procs) byPid[p.pid] = &p;
    for (auto it = m_boosted.begin(); it != m_boosted.end();) {
        if (m_cfg.boost && it->first == fg) { ++it; continue; }
        auto f = byPid.find(it->first);
        if (f != byPid.end()) {
            const ProcessInfo* p = f->second;
            if (p->priorityClass == ABOVE_NORMAL_PRIORITY_CLASS && !IsRuleControlled(p->name) &&
                SetPriorityClass(p->pid, it->second)) {
                ++changed;
                AddLog(L"Active window boost ended for " + p->name, p);
            }
        }
        it = m_boosted.erase(it);
    }
    if (!m_cfg.boost || fg <= 4 || m_boosted.count(fg)) return changed;
    auto f = byPid.find(fg);
    if (f == byPid.end()) return changed;
    const ProcessInfo* p = f->second;
    if (p->priorityClass != NORMAL_PRIORITY_CLASS || IsRuleControlled(p->name) || IsExcluded(p->name)) return changed;
    if (SetPriorityClass(fg, ABOVE_NORMAL_PRIORITY_CLASS)) {
        m_boosted[fg] = NORMAL_PRIORITY_CLASS;
        ++changed;
        AddLog(L"Active window boost: " + p->name, p);
    }
    return changed;
}

// Memory Trim: when memory is high, background processes that are idle get their working set reduced.
int ProcessSupervisor::RunMemoryOptimizer(const std::vector<ProcessInfo>& procs, DWORD fg) {
    if (!m_cfg.memoryOptimizer || m_memLoad < 85.0) return 0;
    ULONGLONG now = GetTickCount64();
    if (now - m_lastFree < 30000) return 0;
    m_lastFree = now;
    return FreeBackgroundMemory(procs) > 0 ? 1 : 0;
}

int ProcessSupervisor::FreeBackgroundMemory(const std::vector<ProcessInfo>& procs) {
    DWORD fg = ForegroundPid();
    DWORD self = GetCurrentProcessId();
    int n = 0;
    for (const ProcessInfo& p : procs) {
        if (p.pid <= 4 || p.pid == self || p.pid == fg || p.cpuPercent >= 0.5) continue;
        if (IsExcluded(p.name)) continue;
        if (FreeProcessMemory(p.pid)) ++n;
    }
    if (n > 0) AddLog(L"Freed memory of " + std::to_wstring(n) + L" background processes");
    return n;
}

bool ProcessSupervisor::FindPlan(const char* keyword, std::wstring& guid) {
    std::string kw = keyword;
    for (const auto& p : ListPowerPlans()) {
        std::wstring lower = LowerW(p.name);
        std::wstring k(kw.begin(), kw.end());
        if (lower.find(k) != std::wstring::npos) { guid = p.guid; return true; }
    }
    return false;
}

// Power plans: a rule's plan wins; otherwise Idle power saver (idle) or high-power mode (busy).
// When none applies, the plan that was active before is restored.
int ProcessSupervisor::RunPowerPlan(const std::vector<ProcessInfo>& procs) {
    std::wstring want;
    for (const ProcessRule& r : m_rules) {
        if (!r.enabled || r.powerPlan.empty()) continue;
        for (const ProcessInfo& p : procs)
            if (p.pid && RuleMatchesProcess(r, p, m_rules)) { want = r.powerPlan; break; }
        if (!want.empty()) break;
    }
    if (want.empty() && m_perfNeeded) PerformancePlanGuid(want);
    if (want.empty() && m_cfg.idleSaver && IdleSeconds() >= (DWORD)(m_cfg.idleMinutes * 60)) FindPlan("saver", want);
    if (want.empty() && m_cfg.performance && m_systemLoad >= m_cfg.loadThreshold) PerformancePlanGuid(want);

    if (!want.empty()) {
        if (!m_planOverridden) GetActivePowerPlan(m_originalPlan);
        m_planOverridden = true;
        std::wstring cur;
        GetActivePowerPlan(cur);
        if (_wcsicmp(cur.c_str(), want.c_str()) == 0) return 0;
        if (SetActivePowerPlan(want)) { AddLog(L"Power plan switched"); return 1; }
        if (_wcsicmp(want.c_str(), m_perfPlanGuid.c_str()) == 0) m_perfPlanGuid.clear();   // plan was deleted
        return 0;
    }
    if (m_planOverridden) {
        m_planOverridden = false;
        if (!m_originalPlan.empty() && SetActivePowerPlan(m_originalPlan)) { AddLog(L"Power plan restored"); return 1; }
    }
    return 0;
}

// ----- keep awake -----
void ProcessSupervisor::SetKeepAwake(int minutes) {
    if (minutes == 0) {
        m_awakeUntil = 0;
        SetThreadExecutionState(ES_CONTINUOUS);
        AddLog(L"Stay awake: off");
    } else {
        m_awakeUntil = minutes < 0 ? ~0ULL : GetTickCount64() + (ULONGLONG)minutes * 60000ULL;
        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
        AddLog(minutes < 0 ? L"Stay awake: indefinitely" : L"Stay awake for " + std::to_wstring(minutes) + L" minutes");
    }
}
void ProcessSupervisor::TickKeepAwake() {
    if (!m_awakeUntil) return;
    if (m_awakeUntil != ~0ULL && GetTickCount64() >= m_awakeUntil) { SetKeepAwake(0); return; }
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
}

// ----- power plans -----
std::vector<PowerPlanInfo> ProcessSupervisor::ListPowerPlans() {
    std::vector<PowerPlanInfo> out;
    for (DWORD i = 0;; ++i) {
        GUID g{};
        DWORD sz = sizeof(g);
        if (PowerEnumerate(nullptr, nullptr, nullptr, ACCESS_SCHEME, i, (UCHAR*)&g, &sz) != ERROR_SUCCESS) break;
        PowerPlanInfo pi;
        pi.guid = GuidToText(g);
        wchar_t name[256] = {};
        DWORD nsz = sizeof(name);
        if (PowerReadFriendlyName(nullptr, &g, nullptr, nullptr, (UCHAR*)name, &nsz) == ERROR_SUCCESS && name[0])
            pi.name = name;
        else pi.name = pi.guid;
        out.push_back(pi);
    }
    return out;
}
bool ProcessSupervisor::GetActivePowerPlan(std::wstring& guid) {
    GUID* g = nullptr;
    if (PowerGetActiveScheme(nullptr, &g) != ERROR_SUCCESS || !g) return false;
    guid = GuidToText(*g);
    LocalFree(g);
    return true;
}
bool ProcessSupervisor::SetActivePowerPlan(const std::wstring& text) {
    GUID g{};
    if (!GuidFromText(text, g)) return false;
    return PowerSetActiveScheme(nullptr, &g) == ERROR_SUCCESS;
}

// ----- configuration -----
bool ProcessSupervisor::LoadConfig() { return LoadConfigFrom(m_configPath); }
bool ProcessSupervisor::SaveConfig() { return SaveConfigTo(m_configPath); }
bool ProcessSupervisor::LoadConfigFrom(const std::wstring& path) {
    std::string text;
    if (!ReadWholeFile(path, text)) return false;
    ParseConfigText(text, m_rules, m_cfg);
    return true;
}
bool ProcessSupervisor::SaveConfigTo(const std::wstring& path) {
    return WriteWholeFile(path, BuildConfigText(m_rules, m_cfg));
}
bool ProcessSupervisor::LoadRulesFrom(const std::wstring& path) {
    std::string text;
    if (!ReadWholeFile(path, text)) return false;
    Settings dummy = m_cfg;
    std::vector<ProcessRule> rules;
    ParseConfigText(text, rules, dummy);
    m_rules = rules;
    return true;
}
bool ProcessSupervisor::SaveRulesTo(const std::wstring& path) {
    std::string o = "{\n  \"application\": \"OpenLasso\",\n  \"rules\": " + RulesArrayText(m_rules) + "\n}\n";
    return WriteWholeFile(path, o);
}
void ProcessSupervisor::ResetConfig() {
    m_rules.clear();
    Settings d;
    m_cfg = d;
}
std::string ProcessSupervisor::RulesToJson(const std::vector<ProcessRule>& rules) {
    return "{\"rules\": " + RulesArrayText(rules) + "}";
}
std::vector<ProcessRule> ProcessSupervisor::RulesFromJson(const std::string& text) {
    std::vector<ProcessRule> r;
    ParseRules(text, r);
    return r;
}
std::vector<std::wstring> ProcessSupervisor::ListProfiles(const std::wstring& dir) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"OpenLasso-*.json").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::wstring n = fd.cFileName;
        if (n.size() > 16) out.push_back(n.substr(10, n.size() - 15));   // OpenLasso-<name>.json
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}
std::wstring ProcessSupervisor::ProfilePath(const std::wstring& name) const {
    if (name.empty()) return m_configPath;
    return ExeDir() + L"OpenLasso-" + name + L".json";
}

// ----- log -----
std::wstring ProcessSupervisor::LogFilePath() const {
    std::wstring dir = m_cfg.logFolder.empty() ? ExeDir() : m_cfg.logFolder + L"\\";
    return dir + L"OpenLasso-log.csv";
}
std::wstring ProcessSupervisor::LogEntryText(const LogEntry& e) {
    std::wstring t = e.time + L" " + e.computer + L" " + e.action;
    if (!e.process.empty()) t += L" " + e.process + L" " + std::to_wstring(e.pid);
    if (!e.path.empty()) t += L" " + e.path;
    if (!e.command.empty()) t += L" " + e.command;
    return t;
}
// Structured log: one record per action with separate computer, process, PID, path and command line.
// The file is CSV (UTF-8, with a header row) and is rotated to <name>.1 when it reaches the size limit.
void ProcessSupervisor::AddLog(const std::wstring& msg, const ProcessInfo* p) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t ts[32];
    swprintf_s(ts, L"%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    LogEntry e;
    e.time = ts;
    e.computer = ComputerNameW();
    e.action = msg;
    if (p) {
        e.process = p->name;
        e.pid = p->pid;
        e.path = p->path;
        e.command = p->cmdLine.empty() ? CommandLineOf(p->pid) : p->cmdLine;
    }
    m_log.push_back(e);
    while (m_log.size() > 1000) m_log.pop_front();
    ++m_logSeq;
    if (!m_cfg.logEnabled) return;

    std::wstring file = LogFilePath();
    ULONGLONG limit = (ULONGLONG)m_cfg.maxLogKB * 1024ULL;
    HANDLE h = CreateFileW(file.c_str(), FILE_APPEND_DATA | GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz = {};
    GetFileSizeEx(h, &sz);
    if ((ULONGLONG)sz.QuadPart >= limit) {
        CloseHandle(h);
        MoveFileExW(file.c_str(), (file + L".1").c_str(), MOVEFILE_REPLACE_EXISTING);
        h = CreateFileW(file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        sz.QuadPart = 0;
    }
    std::string out;
    if (sz.QuadPart == 0) out = "\xEF\xBB\xBF" "time,computer,action,process,pid,path,command\r\n";
    std::wstring row = CsvField(e.time) + L"," + CsvField(e.computer) + L"," + CsvField(e.action) + L"," +
                       CsvField(e.process) + L"," + (e.pid ? std::to_wstring(e.pid) : std::wstring()) + L"," +
                       CsvField(e.path) + L"," + CsvField(e.command) + L"\r\n";
    out += ToUtf8(row);
    DWORD w = 0;
    WriteFile(h, out.data(), (DWORD)out.size(), &w, nullptr);
    CloseHandle(h);
}

std::wstring ProcessSupervisor::SummaryText() const {
    std::wstring t = L"Auto-throttle summary\n\nTotal events: " + std::to_wstring(m_pbTotal) +
                     L"\nEvents today: " + std::to_wstring(m_pbToday) +
                     L"\nProcesses lowered right now: " + std::to_wstring(m_lowered.size()) +
                     L"\nSystem CPU load: " + std::to_wstring((int)(m_systemLoad + 0.5)) + L"%\n\nLowered now:\n";
    ULONGLONG now = GetTickCount64();
    for (const auto& kv : m_lowered) {
        auto it = m_lowerStart.find(kv.first);
        ULONGLONG secs = (it == m_lowerStart.end()) ? 0 : (now - it->second) / 1000;
        t += L"  pid " + std::to_wstring(kv.first) + L" for " + std::to_wstring(secs) + L" s\n";
    }
    if (m_lowered.empty()) t += L"  (none)\n";
    return t;
}

// ----- start with Windows (scheduled task: elevated, at sign-in, no UAC prompt) -----
static bool RunSchtasks(const std::wstring& args) {
    std::wstring cmd = L"schtasks.exe " + args;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
}
bool ProcessSupervisor::GetAutostart() { return RunSchtasks(L"/Query /TN OpenLasso"); }
bool ProcessSupervisor::SetAutostart(bool on, const std::wstring& exePath) {
    if (!on) return RunSchtasks(L"/Delete /TN OpenLasso /F");
    return RunSchtasks(L"/Create /TN OpenLasso /TR \"\\\"" + exePath + L"\\\" /minimized\" /SC ONLOGON /RL HIGHEST /F");
}

// ----- Windows service -----
bool ProcessSupervisor::InstallService() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(path) + L"\" /service";
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) return false;
    SC_HANDLE svc = CreateServiceW(scm, kServiceName, L"OpenLasso Service",
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        cmd.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    bool ok = svc != nullptr;
    if (svc) {
        SERVICE_DESCRIPTIONW d;
        d.lpDescription = const_cast<LPWSTR>(L"Applies OpenLasso priority, affinity and power rules in the background");
        ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &d);
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return ok;
}
bool ProcessSupervisor::UninstallService() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, kServiceName, DELETE | SERVICE_STOP);
    bool ok = false;
    if (svc) {
        SERVICE_STATUS st{};
        ControlService(svc, SERVICE_CONTROL_STOP, &st);
        ok = DeleteService(svc) != FALSE;
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return ok;
}
bool ProcessSupervisor::StartSvc() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_START);
    bool ok = svc && ::StartServiceW(svc, 0, nullptr) != FALSE;
    if (svc) CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}
bool ProcessSupervisor::StopSvc() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_STOP);
    SERVICE_STATUS st{};
    bool ok = svc && ControlService(svc, SERVICE_CONTROL_STOP, &st) != FALSE;
    if (svc) CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}

// ===================== Own performance plan =====================
static const GUID kSubProcessor = { 0x54533251, 0x82be, 0x4824, { 0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00 } };
static const GUID kProcThrottleMin = { 0x893dee8e, 0x2bef, 0x41e0, { 0x89, 0xc6, 0xb5, 0x5d, 0x09, 0x29, 0x96, 0x4c } };
static const GUID kProcThrottleMax = { 0xbc5038f7, 0x23e0, 0x4960, { 0x96, 0xda, 0x33, 0xab, 0xaf, 0x59, 0x35, 0xec } };
static const GUID kPerfBoostMode = { 0xbe337238, 0x0d82, 0x4146, { 0xa9, 0x60, 0x4f, 0x37, 0x49, 0xd4, 0x70, 0xc7 } };
static const GUID kCoreParkMin = { 0x0cc5b647, 0xc1df, 0x4637, { 0x89, 0x1a, 0xde, 0xc3, 0x5c, 0x31, 0x85, 0x83 } };
static const GUID kCoolingPolicy = { 0x94d3a615, 0xa899, 0x4ac5, { 0xae, 0x2b, 0xe4, 0xd8, 0xf6, 0x34, 0x36, 0x7f } };
static const GUID kEpp = { 0x36687f9e, 0xe3a5, 0x4dbf, { 0xb1, 0xdc, 0x15, 0xeb, 0x38, 0x1c, 0x68, 0x63 } };
static const GUID kSubDisk = { 0x0012ee47, 0x9041, 0x4b5d, { 0x9b, 0x77, 0x53, 0x5f, 0xba, 0x8b, 0x14, 0x42 } };
static const GUID kDiskIdle = { 0x6738e2c4, 0xe8a5, 0x4a42, { 0xb1, 0x6a, 0xe0, 0x40, 0xe7, 0x69, 0x75, 0x6e } };
static const GUID kSubUsb = { 0x2a737441, 0x1930, 0x4402, { 0x8d, 0x77, 0xb2, 0xbe, 0xbb, 0xa3, 0x08, 0xa3 } };
static const GUID kUsbSuspend = { 0x48e6b7a6, 0x50f5, 0x4782, { 0xa5, 0xd4, 0x53, 0xbb, 0x8f, 0x07, 0xe2, 0x26 } };
static const GUID kSubPcie = { 0x501a4d13, 0x42af, 0x4429, { 0x9f, 0xd1, 0xa8, 0x21, 0x8c, 0x26, 0x8e, 0x20 } };
static const GUID kAspm = { 0xee12f906, 0xd277, 0x404b, { 0xb6, 0xda, 0xe5, 0xfa, 0x1a, 0x57, 0x6d, 0xf5 } };

static void WritePlanValue(const GUID& plan, const GUID& sub, const GUID& setting, DWORD value) {
    PowerWriteACValueIndex(nullptr, &plan, &sub, &setting, value);     // settings missing on this PC are skipped
    PowerWriteDCValueIndex(nullptr, &plan, &sub, &setting, value);
}

const wchar_t* ProcessSupervisor::PerfPlanName() { return L"OpenLasso High Power"; }

void ProcessSupervisor::ConfigurePerfPlan(const GUID& plan) {
    WritePlanValue(plan, kSubProcessor, kProcThrottleMin, 100);   // minimum processor state 100%
    WritePlanValue(plan, kSubProcessor, kProcThrottleMax, 100);
    WritePlanValue(plan, kSubProcessor, kPerfBoostMode, 2);       // aggressive boost
    WritePlanValue(plan, kSubProcessor, kCoreParkMin, 100);       // no core parking
    WritePlanValue(plan, kSubProcessor, kCoolingPolicy, 1);       // active cooling
    WritePlanValue(plan, kSubProcessor, kEpp, 0);                 // energy/performance preference: performance
    WritePlanValue(plan, kSubDisk, kDiskIdle, 0);                 // never spin the disk down
    WritePlanValue(plan, kSubUsb, kUsbSuspend, 0);                // no USB selective suspend
    WritePlanValue(plan, kSubPcie, kAspm, 0);                     // PCIe link state power management off
}

// Creates the plan the first time (a copy of High performance) and returns its GUID.
bool ProcessSupervisor::EnsurePerformancePlan(std::wstring& guidOut, bool rewrite) {
    if (!m_perfPlanGuid.empty() && !rewrite) { guidOut = m_perfPlanGuid; return true; }
    std::wstring found;
    for (const auto& pl : ListPowerPlans())
        if (_wcsicmp(pl.name.c_str(), PerfPlanName()) == 0) { found = pl.guid; break; }
    bool created = false;
    if (found.empty()) {
        std::wstring srcText;
        if (!FindPlan("high performance", srcText) && !FindPlan("ultimate", srcText)) GetActivePowerPlan(srcText);
        GUID src{};
        if (srcText.empty() || !GuidFromText(srcText, src)) return false;
        GUID* dst = nullptr;
        if (PowerDuplicateScheme(nullptr, &src, &dst) != ERROR_SUCCESS || !dst) return false;
        const wchar_t* nm = PerfPlanName();
        PowerWriteFriendlyName(nullptr, dst, nullptr, nullptr, (UCHAR*)nm, (DWORD)((wcslen(nm) + 1) * sizeof(wchar_t)));
        found = GuidToText(*dst);
        ConfigurePerfPlan(*dst);
        LocalFree(dst);
        created = true;
    } else if (rewrite) {
        GUID g{};
        if (GuidFromText(found, g)) {
            ConfigurePerfPlan(g);
            std::wstring active;
            if (GetActivePowerPlan(active) && _wcsicmp(active.c_str(), found.c_str()) == 0)
                PowerSetActiveScheme(nullptr, &g);                  // re-apply so the new values take effect
        }
    }
    m_perfPlanGuid = found;
    if (created) AddLog(std::wstring(L"Created power plan \"") + PerfPlanName() + L"\"");
    guidOut = found;
    return true;
}
bool ProcessSupervisor::PerformancePlanGuid(std::wstring& guid) {
    if (m_cfg.customPerfPlan && EnsurePerformancePlan(guid)) return true;
    return FindPlan("high performance", guid) || FindPlan("ultimate", guid);
}

// ===================== Keep running (restart with the original command line) =====================
// While a process with a "keep running" rule runs, OpenLasso remembers its path and arguments. When it
// exits (and the user did not end it from OpenLasso) it is started again, as administrator if the rule asks.
// No more than 5 restarts per minute; after that the rule pauses for 5 minutes.
int ProcessSupervisor::RunKeepRunning(const std::vector<ProcessInfo>& procs) {
    if (m_serviceMode) return 0;                      // session 0 cannot show a desktop application
    int n = 0;
    ULONGLONG now = GetTickCount64();
    for (ProcessRule& r : m_rules) {
        if (!r.enabled || !r.keepRunning || r.processName.empty()) continue;
        KeepState& st = m_keep[LowerW(r.processName)];
        const ProcessInfo* found = nullptr;
        for (const ProcessInfo& p : procs)
            if (p.pid > 4 && _wcsicmp(p.name.c_str(), r.processName.c_str()) == 0) { found = &p; break; }
        if (found) {
            if (!st.seen) {
                st.seen = true;
                st.path = found->path;
                if (r.keepRunningPath.empty() && !found->path.empty()) r.keepRunningPath = found->path;
                if (r.keepRunningCommandLine.empty()) st.args = StripArgv0(CommandLineOf(found->pid));
            }
            continue;
        }
        if (st.userEnded) { st.userEnded = false; st.seen = false; continue; }
        if (!st.seen) continue;
        st.seen = false;                              // it just exited
        if (now < st.pausedUntil) continue;
        st.restarts.erase(std::remove_if(st.restarts.begin(), st.restarts.end(),
                                         [&](ULONGLONG t) { return now - t > 60000; }), st.restarts.end());
        if (st.restarts.size() >= 5) {
            st.pausedUntil = now + 300000;
            AddLog(L"Keep running: " + r.processName + L" keeps exiting; paused for 5 minutes");
            continue;
        }
        std::wstring path = r.keepRunningPath.empty() ? st.path : r.keepRunningPath;
        std::wstring args = r.keepRunningCommandLine.empty() ? st.args : r.keepRunningCommandLine;
        if (path.empty()) continue;
        bool ok = LaunchProcess(path, args, r.keepRunningElevate);
        st.restarts.push_back(now);
        AddLog(std::wstring(ok ? L"Keep running: restarted " : L"Keep running: could not restart ") + r.processName +
               (args.empty() ? L"" : L" with its arguments"));
        ++n;
    }
    return n;
}

// ===================== CPU Sets, GPU, sleep and performance holds =====================
static std::vector<ULONG> CpuSetIds() {
    std::vector<ULONG> ids;
    ULONG len = 0;
    GetSystemCpuSetInformation(nullptr, 0, &len, GetCurrentProcess(), 0);
    if (!len) return ids;
    std::vector<BYTE> buf(len);
    if (!GetSystemCpuSetInformation((SYSTEM_CPU_SET_INFORMATION*)buf.data(), len, &len, GetCurrentProcess(), 0))
        return ids;
    BYTE* p = buf.data();
    while (p < buf.data() + len) {
        SYSTEM_CPU_SET_INFORMATION* e = (SYSTEM_CPU_SET_INFORMATION*)p;
        if (e->Size == 0) break;
        if (e->Type == CpuSetInformation) ids.push_back(e->CpuSet.Id);
        p += e->Size;
    }
    return ids;
}

// mode: 1 = all CPUs (clears the selection), 2 = first half, 3 = second half.
// CPU Sets are a soft preference: the scheduler uses these CPUs first.
bool ProcessSupervisor::SetCpuSets(DWORD pid, int mode) {
    std::vector<ULONG> ids = CpuSetIds();
    if (ids.empty()) return false;
    std::vector<ULONG> sel;
    size_t half = ids.size() / 2;
    if (mode == 2 && half) sel.assign(ids.begin(), ids.begin() + half);
    else if (mode == 3 && half) sel.assign(ids.begin() + half, ids.end());
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    const ULONG* data = sel.empty() ? nullptr : sel.data();
    ULONG count = (ULONG)sel.size();
    BOOL ok = SetProcessDefaultCpuSets(h, data, count);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        if (Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID != pid) continue;
                HANDLE th = OpenThread(THREAD_SET_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
                if (th) {
                    SetThreadSelectedCpuSets(th, data, count);
                    CloseHandle(th);
                }
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
    }
    CloseHandle(h);
    return ok != FALSE;
}

// ---------------- GPU priority (D3DKMT scheduling class) ----------------
// The GPU scheduler belongs to the WDDM driver, so the request can be refused or silently ignored.
// OpenLasso therefore (1) checks that WDDM is present, (2) reads the class back to see if it was kept and
// (3) can fall back to the per-application GPU preference (see ApplyGpuRule).
namespace {
typedef LONG(WINAPI* PFN_KMT_SET)(HANDLE, int);
typedef LONG(WINAPI* PFN_KMT_GET)(HANDLE, int*);
struct KmtEnumAdapters2 { ULONG NumAdapters; void* pAdapters; };
typedef LONG(WINAPI* PFN_KMT_ENUM2)(KmtEnumAdapters2*);
struct KmtApi {
    PFN_KMT_SET set = nullptr;
    PFN_KMT_GET get = nullptr;
    bool wddm = false;
};
KmtApi& Kmt() {
    static KmtApi api = []() {
        KmtApi a;
        HMODULE gdi = LoadLibraryW(L"gdi32.dll");
        if (!gdi) return a;
        a.set = (PFN_KMT_SET)GetProcAddress(gdi, "D3DKMTSetProcessSchedulingPriorityClass");
        a.get = (PFN_KMT_GET)GetProcAddress(gdi, "D3DKMTGetProcessSchedulingPriorityClass");
        PFN_KMT_ENUM2 en = (PFN_KMT_ENUM2)GetProcAddress(gdi, "D3DKMTEnumAdapters2");
        if (a.set) {
            a.wddm = true;                                  // API present (WDDM 1.x+)
            if (en) {                                       // no adapter at all -> no GPU scheduling
                KmtEnumAdapters2 q{};
                if (en(&q) == 0 && q.NumAdapters == 0) a.wddm = false;
            }
        }
        return a;
    }();
    return api;
}
const wchar_t* kGpuNames[6] = { L"Idle", L"Below normal", L"Normal", L"Above normal", L"High", L"Realtime" };
}

bool ProcessSupervisor::GpuSchedulingSupported() { return Kmt().wddm; }

const wchar_t* ProcessSupervisor::GpuResultText(GpuResult r) {
    switch (r) {
    case GpuResult::Applied: return L"applied";
    case GpuResult::Unsupported: return L"this system has no WDDM GPU scheduling";
    case GpuResult::Denied: return L"access denied (protected process or missing privilege)";
    case GpuResult::Ignored: return L"the GPU driver ignored the request";
    default: return L"the call failed";
    }
}

GpuResult ProcessSupervisor::SetGpuPriorityEx(DWORD pid, DWORD v) {
    KmtApi& api = Kmt();
    if (!api.set || !api.wddm) return GpuResult::Unsupported;
    if (v > 5) return GpuResult::Failed;
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION,
                           FALSE, pid);
    if (!h) h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
    if (!h) return GpuResult::Denied;
    LONG st = api.set(h, (int)v);
    GpuResult res = GpuResult::Applied;
    if (st != 0) {
        res = (st == (LONG)0xC0000022 || st == (LONG)0xC0000061) ? GpuResult::Denied : GpuResult::Failed;
    } else if (api.get) {
        int cur = -1;
        if (api.get(h, &cur) == 0 && cur != (int)v) res = GpuResult::Ignored;   // call succeeded, value did not stick
    }
    CloseHandle(h);
    return res;
}
bool ProcessSupervisor::SetGpuPriority(DWORD pid, DWORD v) { return SetGpuPriorityEx(pid, v) == GpuResult::Applied; }

// Applies a rule's GPU priority, explains a refusal and (optionally) falls back to GPU preference.
GpuResult ProcessSupervisor::ApplyGpuRule(const ProcessInfo& p, const ProcessRule& r) {
    DWORD v = (DWORD)(r.gpuPriority - 1);
    GpuResult res = SetGpuPriorityEx(p.pid, v);
    int key = (int)res;
    auto it = m_gpuLogged.find(p.pid);
    bool report = (it == m_gpuLogged.end() || it->second != key);
    m_gpuLogged[p.pid] = key;
    const wchar_t* label = v < 6 ? kGpuNames[v] : L"?";
    if (res == GpuResult::Applied) {
        if (report) AddLog(std::wstring(L"GPU priority ") + label + L" set for " + p.name, &p);
        return res;
    }
    std::wstring msg = std::wstring(L"GPU priority ") + label + L" not applied for " + p.name + L": " + GpuResultText(res);
    if (!r.gpuPref && m_cfg.gpuFallback && (res == GpuResult::Ignored || res == GpuResult::Unsupported) &&
        !p.path.empty() && (v >= 3 || v <= 1)) {
        int reg = (v >= 3) ? 2 : 1;                         // high performance / power saving
        if (SetGpuPreference(p.path, reg))
            msg += std::wstring(L". Fallback: GPU preference set to ") +
                   (reg == 2 ? L"High performance" : L"Power saving") + L" (takes effect at the next launch)";
    }
    m_lastNote = msg;
    if (report) AddLog(msg, &p);
    return res;
}

// ---------------- GPU load and memory (PDH "GPU Engine" and "GPU Process Memory") ----------------
void ProcessSupervisor::ClosePdh() {
    if (m_pdhQuery) PdhCloseQuery(m_pdhQuery);
    m_pdhQuery = m_pdhUtil = m_pdhMem = nullptr;
}
// "pid_1234_luid_0x..._phys_0_eng_0_engtype_3D" -> pid 1234, rest "luid_..._engtype_3D"
static bool GpuInstancePid(const std::wstring& n, DWORD& pid, std::wstring& rest) {
    if (n.compare(0, 4, L"pid_") != 0) return false;
    size_t e = n.find(L'_', 4);
    if (e == std::wstring::npos) return false;
    pid = (DWORD)wcstoul(n.substr(4, e - 4).c_str(), nullptr, 10);
    rest = n.substr(e + 1);
    return true;
}
static bool PdhItems(HANDLE counter, std::vector<BYTE>& buf, DWORD& count) {
    DWORD bytes = 0;
    count = 0;
    PDH_STATUS st = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, nullptr);
    if (st != PDH_MORE_DATA || !bytes) return false;
    buf.assign(bytes, 0);
    st = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count,
                                      (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data());
    return st == ERROR_SUCCESS;
}
void ProcessSupervisor::SampleGpu() {
    ULONGLONG now = GetTickCount64();
    if (m_pdhQuery && now - m_pdhOpened > 60000) ClosePdh();           // re-read the instance list now and then
    if (!m_pdhQuery) {
        if (m_pdhFailed && now - m_pdhOpened < 300000) return;         // counters missing: retry every 5 minutes
        m_pdhOpened = now;
        PDH_HQUERY q = nullptr;
        if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) { m_pdhFailed = true; return; }
        PDH_HCOUNTER cu = nullptr, cm = nullptr;
        if (PdhAddEnglishCounterW(q, L"\\GPU Engine(*)\\Utilization Percentage", 0, &cu) != ERROR_SUCCESS) {
            PdhCloseQuery(q);
            m_pdhFailed = true;
            return;
        }
        PdhAddEnglishCounterW(q, L"\\GPU Process Memory(*)\\Dedicated Usage", 0, &cm);   // optional
        m_pdhQuery = q;
        m_pdhUtil = cu;
        m_pdhMem = cm;
        m_pdhFailed = false;
        PdhCollectQueryData(q);                                        // rates need two samples
        return;
    }
    if (PdhCollectQueryData(m_pdhQuery) != ERROR_SUCCESS) return;

    std::vector<BYTE> buf;
    DWORD count = 0;
    if (PdhItems(m_pdhUtil, buf, count)) {
        std::map<std::wstring, double> engine;                         // whole system, per physical engine
        std::map<DWORD, std::map<std::wstring, double>> perProc;
        PDH_FMT_COUNTERVALUE_ITEM_W* it = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
        for (DWORD i = 0; i < count; ++i) {
            if (it[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && it[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
            DWORD pid = 0;
            std::wstring rest;
            if (!it[i].szName || !GpuInstancePid(it[i].szName, pid, rest)) continue;
            double v = it[i].FmtValue.doubleValue;
            engine[rest] += v;
            perProc[pid][rest] += v;
        }
        // like Task Manager: the busiest engine represents the load
        double sys = 0.0;
        for (const auto& kv : engine) sys = (std::max)(sys, kv.second);
        std::map<DWORD, double> byPid;
        for (const auto& pp : perProc) {
            double mx = 0.0;
            for (const auto& kv : pp.second) mx = (std::max)(mx, kv.second);
            byPid[pp.first] = (std::min)(mx, 100.0);
        }
        m_gpuLoad = (std::min)(sys, 100.0);
        m_gpuPid.swap(byPid);
        m_gpuAvail = true;
    }
    if (m_pdhMem && PdhItems(m_pdhMem, buf, count)) {
        std::map<DWORD, ULONGLONG> mem;
        ULONGLONG total = 0;
        PDH_FMT_COUNTERVALUE_ITEM_W* it = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
        for (DWORD i = 0; i < count; ++i) {
            if (it[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && it[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
            DWORD pid = 0;
            std::wstring rest;
            if (!it[i].szName || !GpuInstancePid(it[i].szName, pid, rest)) continue;
            ULONGLONG b = (ULONGLONG)(std::max)(0.0, it[i].FmtValue.doubleValue);
            mem[pid] += b;
            total += b;
        }
        m_gpuMem.swap(mem);
        m_gpuMemTotal = total;
    }
}

// GPU preference: per-application setting for the graphics adapter (applies to the next launch).
bool ProcessSupervisor::SetGpuPreference(const std::wstring& exe, int regValue) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\DirectX\\UserGpuPreferences", 0, nullptr, 0,
                        KEY_READ | KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;
    bool ok = true;
    if (regValue == 0) {
        RegDeleteValueW(k, exe.c_str());
    } else {
        std::wstring val = (regValue == 1) ? L"GpuPreference=1;" : L"GpuPreference=2;";
        wchar_t cur[128] = {};
        DWORD sz = sizeof(cur), type = 0;
        bool same = RegQueryValueExW(k, exe.c_str(), nullptr, &type, (LPBYTE)cur, &sz) == ERROR_SUCCESS && val == cur;
        if (!same)
            ok = RegSetValueExW(k, exe.c_str(), 0, REG_SZ, (const BYTE*)val.c_str(),
                                (DWORD)((val.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    }
    RegCloseKey(k);
    return ok;
}

// Holds: Block Sleep and Force High-Power Plan, from the menu (runtime) or from rules.
void ProcessSupervisor::SetSleepHold(DWORD pid, bool on) {
    if (on) m_sleepPids.insert(pid); else m_sleepPids.erase(pid);
}
void ProcessSupervisor::SetPerfHold(DWORD pid, bool on) {
    if (on) m_perfPids.insert(pid); else m_perfPids.erase(pid);
}

int ProcessSupervisor::RunHolds(const std::vector<ProcessInfo>& procs) {
    std::set<DWORD> alive;
    for (const auto& p : procs) alive.insert(p.pid);
    auto prune = [&](auto& container) {
        for (auto it = container.begin(); it != container.end();)
            it = alive.count(it->first) ? std::next(it) : container.erase(it);
    };
    auto pruneSet = [&](std::set<DWORD>& s) {
        for (auto it = s.begin(); it != s.end();) it = alive.count(*it) ? std::next(it) : s.erase(it);
    };
    pruneSet(m_sleepPids);
    pruneSet(m_perfPids);
    prune(m_appliedCpu);
    prune(m_appliedBoost);
    prune(m_appliedPref);
    prune(m_appliedGpu);
    prune(m_gpuLogged);

    bool sleep = !m_sleepPids.empty();
    bool perf = !m_perfPids.empty();
    for (const ProcessRule& r : m_rules) {
        if (!r.enabled || r.processName.empty() || (!r.preventSleep && !r.forcePower)) continue;
        for (const ProcessInfo& p : procs)
            if (RuleMatchesProcess(r, p, m_rules)) {
                if (r.preventSleep) sleep = true;
                if (r.forcePower) perf = true;
            }
    }
    m_perfNeeded = perf;
    if (sleep != m_sleepHeldNow) {
        m_sleepHeldNow = sleep;
        if (sleep) SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
        else if (!m_awakeUntil) SetThreadExecutionState(ES_CONTINUOUS);
        AddLog(sleep ? L"Block sleep: on" : L"Block sleep: off");
    }
    return 0;
}
