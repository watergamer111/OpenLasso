// features.cpp - OpenLasso additions: rule matching (name, full path, folder, wildcards,
// default rule), CPU cap, CPU guard, block, instance spreading, per-core / disk / network
// sampling, service heartbeat, tray notifications, throttle CSV report and command line.
#include "process_supervisor.h"
#include <pdh.h>
#include <pdhmsg.h>
#include <shellapi.h>
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <set>
#include <string>
#include <vector>

// ---------- glob and rule matching ----------
static std::wstring LowerTxt(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}
static bool GlobRec(const wchar_t* p, const wchar_t* t) {
    while (*p) {
        if (*p == L'*') {
            while (*p == L'*') ++p;
            if (!*p) return true;
            for (const wchar_t* s = t;; ++s) {
                if (GlobRec(p, s)) return true;
                if (!*s) return false;
            }
        }
        if (!*t) return false;
        if (*p != L'?' && towlower(*p) != towlower(*t)) return false;
        ++p;
        ++t;
    }
    return *t == 0;
}
bool MatchGlob(const std::wstring& pattern, const std::wstring& text) {
    return GlobRec(pattern.c_str(), text.c_str());
}

// "*"          -> default rule: only processes that no other rule covers
// "C:\dir\"    -> every program in that folder (trailing backslash)
// "C:\dir\a.exe" or any pattern with a backslash -> full path, wildcards allowed
// "chrome.exe" or "chrom*.exe" -> process name, wildcards allowed
bool RuleMatchesProcess(const ProcessRule& r, const ProcessInfo& p, const std::vector<ProcessRule>& all) {
    if (!r.enabled || r.processName.empty()) return false;
    if (r.processName == L"*") {
        for (const auto& o : all) {
            if (!o.enabled || o.processName.empty() || o.processName == L"*") continue;
            if (RuleMatchesProcess(o, p, all)) return false;
        }
        return true;
    }
    const std::wstring& pat = r.processName;
    if (pat.find(L'\\') != std::wstring::npos || pat.find(L'/') != std::wstring::npos) {
        if (pat.back() == L'\\' || pat.back() == L'/')
            return LowerTxt(p.path).compare(0, pat.size(), LowerTxt(pat)) == 0;
        return MatchGlob(pat, p.path);
    }
    return MatchGlob(pat, p.name);
}

// ---------- helpers ----------
static std::string ToU8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static DWORD_PTR SysAffinity() {
    DWORD_PTR pm = 0, sm = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm);
    return sm;
}
// Suspends or resumes every thread of a process (documented SuspendThread / ResumeThread).
static void SetThreadsSuspended(DWORD pid, bool suspend) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            HANDLE th = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
            if (th) {
                if (suspend) SuspendThread(th);
                else ResumeThread(th);
                CloseHandle(th);
            }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

// ---------- engine members ----------
int ProcessSupervisor::RunFeatures(const std::vector<ProcessInfo>& procs) {
    std::set<DWORD> alive;
    for (const auto& p : procs) alive.insert(p.pid);
    for (auto it = m_hot.begin(); it != m_hot.end();) it = alive.count(it->first) ? std::next(it) : m_hot.erase(it);
    for (auto it = m_wdSince.begin(); it != m_wdSince.end();) it = alive.count(it->first) ? std::next(it) : m_wdSince.erase(it);
    for (auto it = m_wdFired.begin(); it != m_wdFired.end();) it = alive.count(*it) ? std::next(it) : m_wdFired.erase(it);
    int n = RunBlock(procs);
    n += RunGuard(procs);
    n += RunCap(procs);
    n += RunSpread(procs);
    return n;
}

// Blocked processes are closed as soon as they are seen.
int ProcessSupervisor::RunBlock(const std::vector<ProcessInfo>& procs) {
    int changed = 0;
    DWORD self = GetCurrentProcessId();
    for (const ProcessRule& r : m_rules) {
        if (!r.enabled || !r.block || r.processName.empty()) continue;
        for (const ProcessInfo& p : procs) {
            if (p.pid <= 4 || p.pid == self || !RuleMatchesProcess(r, p, m_rules)) continue;
            if (EndProcess(p.pid)) {
                ++changed;
                AddLog(L"Blocked process closed: " + p.name, &p);
                Notify(L"Closed blocked program: " + p.name);
            }
        }
    }
    return changed;
}

// CPU guard: after N seconds above the CPU threshold, lower priority, close or run a command.
int ProcessSupervisor::RunGuard(const std::vector<ProcessInfo>& procs) {
    int changed = 0;
    DWORD self = GetCurrentProcessId();
    ULONGLONG now = GetTickCount64();
    for (const ProcessRule& r : m_rules) {
        if (!r.enabled || r.guardCpu <= 0 || r.guardAction == 0 || r.processName.empty()) continue;
        for (const ProcessInfo& p : procs) {
            if (p.pid <= 4 || p.pid == self || !RuleMatchesProcess(r, p, m_rules)) continue;
            if (p.cpuPercent < r.guardCpu) {
                m_wdSince.erase(p.pid);
                m_wdFired.erase(p.pid);
                continue;
            }
            auto it = m_wdSince.find(p.pid);
            if (it == m_wdSince.end()) { m_wdSince[p.pid] = now; continue; }
            if (m_wdFired.count(p.pid) || now - it->second < (ULONGLONG)r.guardSeconds * 1000ULL) continue;
            m_wdFired.insert(p.pid);
            std::wstring what;
            if (r.guardAction == 1) {
                if (SetPriorityClass(p.pid, BELOW_NORMAL_PRIORITY_CLASS)) what = L"priority lowered";
            } else if (r.guardAction == 2) {
                if (EndProcess(p.pid)) what = L"closed";
            } else if (r.guardAction == 3 && !r.guardCommand.empty()) {
                if ((INT_PTR)ShellExecuteW(nullptr, L"open", L"cmd.exe", (L"/c " + r.guardCommand).c_str(), nullptr, SW_HIDE) > 32)
                    what = L"ran its command";
            }
            if (!what.empty()) {
                ++changed;
                AddLog(L"CPU guard: " + p.name + L" " + what, &p);
                Notify(L"CPU guard: " + p.name + L" " + what);
            }
        }
    }
    return changed;
}

// CPU cap: a process above its limit has its threads paused for the excess share of the next second.
int ProcessSupervisor::RunCap(const std::vector<ProcessInfo>& procs) {
    int changed = 0;
    DWORD self = GetCurrentProcessId();
    ULONGLONG now = GetTickCount64();
    std::set<DWORD> alive;
    for (const auto& p : procs) alive.insert(p.pid);
    for (auto it = m_limitResume.begin(); it != m_limitResume.end();) {
        DWORD pid = it->first;
        if (!alive.count(pid) || now >= it->second) {
            if (alive.count(pid)) { SetThreadsSuspended(pid, false); ++changed; }
            it = m_limitResume.erase(it);
        } else ++it;
    }
    for (const ProcessRule& r : m_rules) {
        if (!r.enabled || r.limitPercent <= 0 || r.processName.empty()) continue;
        for (const ProcessInfo& p : procs) {
            if (p.pid <= 4 || p.pid == self || m_limitResume.count(p.pid)) continue;
            if (!RuleMatchesProcess(r, p, m_rules)) continue;
            double limit = (double)r.limitPercent;
            if (p.cpuPercent <= limit || p.cpuPercent <= 0.0) continue;
            double ratio = std::clamp((p.cpuPercent - limit) / p.cpuPercent, 0.0, 0.95);
            SetThreadsSuspended(p.pid, true);
            m_limitResume[p.pid] = now + (ULONGLONG)(ratio * 1000.0);
            ++changed;
        }
    }
    return changed;
}
void ProcessSupervisor::ResumeLimited() {
    for (const auto& kv : m_limitResume) SetThreadsSuspended(kv.first, false);
    m_limitResume.clear();
}

// Instance spreading: several copies of the same program get different CPUs (round robin).
int ProcessSupervisor::RunSpread(const std::vector<ProcessInfo>& procs) {
    int changed = 0;
    DWORD self = GetCurrentProcessId();
    DWORD_PTR sm = SysAffinity();
    std::vector<DWORD_PTR> cpus;
    for (int bit = 0; bit < 64; ++bit)
        if (sm & ((DWORD_PTR)1 << bit)) cpus.push_back((DWORD_PTR)1 << bit);
    if (cpus.empty()) return 0;
    for (const ProcessRule& r : m_rules) {
        if (!r.enabled || !r.balanceInstances || r.affinityMask || r.processName.empty()) continue;
        std::vector<const ProcessInfo*> inst;
        for (const ProcessInfo& p : procs)
            if (p.pid > 4 && p.pid != self && RuleMatchesProcess(r, p, m_rules)) inst.push_back(&p);
        if (inst.size() < 2) continue;
        std::sort(inst.begin(), inst.end(), [](const ProcessInfo* a, const ProcessInfo* b) { return a->pid < b->pid; });
        for (size_t i = 0; i < inst.size(); ++i) {
            DWORD_PTR mask = cpus[i % cpus.size()];
            if (inst[i]->affinityMask != mask && SetAffinityMask(inst[i]->pid, mask)) ++changed;
        }
    }
    return changed;
}

// ---------- sampling: per-core CPU, disk and network ----------
void ProcessSupervisor::SampleExtras() {
    struct SppiLocal {
        LARGE_INTEGER IdleTime, KernelTime, UserTime, DpcTime, InterruptTime;
        ULONG InterruptCount;
    };
    typedef LONG(NTAPI * PFN_QSI)(ULONG, PVOID, ULONG, PULONG);
    static PFN_QSI qsi = (PFN_QSI)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    size_t n = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
    if (qsi) {
        std::vector<SppiLocal> buf(n);
        ULONG ret = 0;
        if (qsi(8, buf.data(), (ULONG)(n * sizeof(SppiLocal)), &ret) == 0) {
            size_t got = (std::min)(n, (size_t)ret / sizeof(SppiLocal));
            bool first = m_prevCoreIdle.size() != got;
            if (first) {
                m_prevCoreIdle.assign(got, 0);
                m_prevCoreKern.assign(got, 0);
                m_prevCoreUser.assign(got, 0);
                m_coreLoad.assign(got, 0.0);
            }
            for (size_t i = 0; i < got; ++i) {
                ULONGLONG idle = (ULONGLONG)buf[i].IdleTime.QuadPart;
                ULONGLONG kern = (ULONGLONG)buf[i].KernelTime.QuadPart;
                ULONGLONG user = (ULONGLONG)buf[i].UserTime.QuadPart;
                if (!first) {
                    ULONGLONG di = idle - m_prevCoreIdle[i];
                    ULONGLONG total = (kern - m_prevCoreKern[i]) + (user - m_prevCoreUser[i]);
                    if (total > 0 && di <= total) m_coreLoad[i] = 100.0 * (double)(total - di) / (double)total;
                }
                m_prevCoreIdle[i] = idle;
                m_prevCoreKern[i] = kern;
                m_prevCoreUser[i] = user;
            }
        }
    }
    ULONGLONG now = GetTickCount64();
    if (now >= m_ioNext) {
        m_ioNext = now + 1000;
        SampleIo();
    }
}

void ProcessSupervisor::SampleIo() {
    if (!m_ioQuery) {
        if (PdhOpenQueryW(nullptr, 0, &m_ioQuery) != ERROR_SUCCESS) { m_ioQuery = nullptr; return; }
        PdhAddEnglishCounterW(m_ioQuery, L"\\PhysicalDisk(_Total)\\% Disk Time", 0, &m_ioDisk);
        PdhAddEnglishCounterW(m_ioQuery, L"\\Network Interface(*)\\Bytes Total/sec", 0, &m_ioNet);
        PdhCollectQueryData(m_ioQuery);       // baseline: rates need two samples
        return;
    }
    if (PdhCollectQueryData(m_ioQuery) != ERROR_SUCCESS) return;
    PDH_FMT_COUNTERVALUE v{};
    if (m_ioDisk && PdhGetFormattedCounterValue(m_ioDisk, PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS &&
        v.CStatus == PDH_CSTATUS_VALID_DATA)
        m_diskPct = std::clamp(v.doubleValue, 0.0, 100.0);
    if (m_ioNet) {
        DWORD bufSize = 0, count = 0;
        if (PdhGetFormattedCounterArrayW(m_ioNet, PDH_FMT_DOUBLE, &bufSize, &count, nullptr) == PDH_MORE_DATA && bufSize) {
            std::vector<BYTE> buf(bufSize);
            PDH_FMT_COUNTERVALUE_ITEM_W* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
            if (PdhGetFormattedCounterArrayW(m_ioNet, PDH_FMT_DOUBLE, &bufSize, &count, items) == ERROR_SUCCESS) {
                double sum = 0.0;
                for (DWORD i = 0; i < count; ++i)
                    if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA) sum += items[i].FmtValue.doubleValue;
                m_netKBps = sum / 1024.0;
            }
        }
    }
}
void ProcessSupervisor::CloseIoPdh() {
    if (m_ioQuery) PdhCloseQuery(m_ioQuery);
    m_ioQuery = nullptr;
    m_ioDisk = nullptr;
    m_ioNet = nullptr;
}

// ---------- service heartbeat (the window reads it to show the service state) ----------
void ProcessSupervisor::ServiceHeartbeat() {
    std::wstring f = ExeDir() + L"OpenLasso-service.status";
    HANDLE h = CreateFileW(f.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string s = "running\n";
    DWORD w = 0;
    WriteFile(h, s.data(), (DWORD)s.size(), &w, nullptr);
    CloseHandle(h);
}
bool ProcessSupervisor::ServiceRunning() const {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW((ExeDir() + L"OpenLasso-service.status").c_str(), GetFileExInfoStandard, &d)) return false;
    FILETIME nowFt{};
    GetSystemTimeAsFileTime(&nowFt);
    ULARGE_INTEGER a{}, b{};
    a.LowPart = d.ftLastWriteTime.dwLowDateTime;
    a.HighPart = d.ftLastWriteTime.dwHighDateTime;
    b.LowPart = nowFt.dwLowDateTime;
    b.HighPart = nowFt.dwHighDateTime;
    return b.QuadPart >= a.QuadPart && (b.QuadPart - a.QuadPart) < 50000000ULL;   // fresh within 5 s
}

// ---------- notifications and Auto-throttle report ----------
void ProcessSupervisor::Notify(const std::wstring& text) {
    if (!m_cfg.notifications) return;
    m_notes.push_back(text);
    if (m_notes.size() > 20) m_notes.erase(m_notes.begin());
}
std::vector<std::wstring> ProcessSupervisor::TakeNotes() {
    std::vector<std::wstring> v;
    v.swap(m_notes);
    return v;
}
std::wstring ProcessSupervisor::ThrottleReportPath() const {
    return (m_cfg.logFolder.empty() ? ExeDir() : m_cfg.logFolder + L"\\") + L"OpenLasso-throttle.csv";
}
void ProcessSupervisor::RecordStat(const std::wstring& name, double cpu) {
    std::wstring file = ThrottleReportPath();
    bool exists = GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES;
    HANDLE h = CreateFileW(file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        std::string out;
        if (!exists) out += "\xEF\xBB\xBF" "time,process,cpu_percent\r\n";
        SYSTEMTIME st{};
        GetLocalTime(&st);
        char line[600];
        snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d,\"%s\",%.1f\r\n", st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond, ToU8(name).c_str(), cpu);
        out += line;
        DWORD w = 0;
        WriteFile(h, out.data(), (DWORD)out.size(), &w, nullptr);
        CloseHandle(h);
    }
    Notify(L"Auto-throttle lowered " + name);
}

// ---------- command line: OpenLasso.exe --version | --status | --list | --apply | --help ----------
static void WriteOut(const std::wstring& text) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    if (!WriteConsoleW(out, text.c_str(), (DWORD)text.size(), &w, nullptr)) {
        std::string u8 = ToU8(text);
        WriteFile(out, u8.data(), (DWORD)u8.size(), &w, nullptr);
    }
}
static bool HasSwitch(const std::wstring& args, const wchar_t* sw) {
    return args.find(sw) != std::wstring::npos;
}

int RunCommandLine(const std::wstring& args, bool& handled) {
    const bool any = HasSwitch(args, L"--version") || HasSwitch(args, L"--status") || HasSwitch(args, L"--list") ||
                     HasSwitch(args, L"--apply") || HasSwitch(args, L"--help");
    handled = any;
    if (!any) return 0;
    AttachConsole(ATTACH_PARENT_PROCESS);

    std::wstring out;
    if (HasSwitch(args, L"--help")) {
        out += L"OpenLasso 1.0 by watergamer111\r\n"
               L"  --version            show the version\r\n"
               L"  --status             service state and number of rules\r\n"
               L"  --list               list processes with CPU and priority\r\n"
               L"  --apply              apply rules and settings once\r\n"
               L"  --profile <name>     use a saved profile (with --apply or --list)\r\n";
    }
    if (HasSwitch(args, L"--version")) out += L"OpenLasso 1.0 by watergamer111\r\n";

    ProcessSupervisor sv;
    sv.LoadConfig();
    size_t pp = args.find(L"--profile ");
    if (pp != std::wstring::npos) {
        size_t s = pp + 10;
        while (s < args.size() && args[s] == L' ') ++s;
        size_t e = args.find(L' ', s);
        std::wstring name = args.substr(s, e == std::wstring::npos ? std::wstring::npos : e - s);
        if (!name.empty() && !sv.LoadConfigFrom(sv.ProfilePath(name)))
            out += L"Profile not found: " + name + L"\r\n";
    }
    if (HasSwitch(args, L"--status")) {
        out += std::wstring(L"Service: ") + (sv.ServiceRunning() ? L"running" : L"stopped") + L"\r\n";
        out += L"Rules: " + std::to_wstring(sv.Rules().size()) + L"\r\n";
    }
    if (HasSwitch(args, L"--list")) {
        sv.SampleSystemLoad();
        sv.EnumerateProcesses();
        Sleep(1100);
        sv.SampleSystemLoad();
        std::vector<ProcessInfo> procs = sv.EnumerateProcesses();
        for (const auto& p : procs) {
            wchar_t line[512];
            swprintf_s(line, L"%-32s %6u  %5.1f%%  %s\r\n", p.name.c_str(), (unsigned)p.pid, p.cpuPercent,
                       PriorityToLabel(p.priorityClass).c_str());
            out += line;
        }
    }
    if (HasSwitch(args, L"--apply")) {
        sv.SampleSystemLoad();
        std::vector<ProcessInfo> procs = sv.EnumerateProcesses();
        int n = sv.ApplyAll(procs);
        out += L"Applied: " + std::to_wstring(n) + L" change(s)\r\n";
    }
    WriteOut(out);
    return 0;
}

// ===================== Smoothness index =====================
// A background thread asks the scheduler to wake it every 10 ms and measures how late it wakes up.
// Average lateness of 2 ms or less = 100 %, 50 ms or more = 0 %.
static volatile LONG g_avgLateUs = 0;
static volatile LONG g_probeRun = 0;
static HANDLE g_probeThread = nullptr;

static DWORD WINAPI ProbeThread(LPVOID) {
    HANDLE timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    if (!timer) return 0;
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    double hist[10] = {};
    int idx = 0;
    while (InterlockedCompareExchange(&g_probeRun, 1, 1) == 1) {
        LARGE_INTEGER t0{}, t1{}, due{};
        QueryPerformanceCounter(&t0);
        due.QuadPart = -100000LL;                     // 10 ms, relative
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        WaitForSingleObject(timer, 1000);
        QueryPerformanceCounter(&t1);
        double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
        double late = ms - 10.0;
        if (late < 0.0) late = 0.0;
        hist[idx % 10] = late;
        ++idx;
        double sum = 0.0;
        int n = idx < 10 ? idx : 10;
        for (int i = 0; i < n; ++i) sum += hist[i];
        InterlockedExchange(&g_avgLateUs, (LONG)((sum / n) * 1000.0));
        Sleep(90);
    }
    CloseHandle(timer);
    return 0;
}
void StartSmoothness() {
    if (g_probeThread) return;
    InterlockedExchange(&g_probeRun, 1);
    g_probeThread = CreateThread(nullptr, 0, ProbeThread, nullptr, 0, nullptr);
}
void StopSmoothness() {
    InterlockedExchange(&g_probeRun, 0);
    if (g_probeThread) {
        WaitForSingleObject(g_probeThread, 2000);
        CloseHandle(g_probeThread);
        g_probeThread = nullptr;
    }
}
int SmoothnessIndex() {
    double avgMs = InterlockedCompareExchange(&g_avgLateUs, 0, 0) / 1000.0;
    double v = 100.0 * (50.0 - avgMs) / 48.0;
    return (int)std::clamp(v + 0.5, 0.0, 100.0);
}

// ===================== Processor groups (more than 64 logical CPUs) =====================
int ProcessSupervisor::LogicalProcessorCount() {
    return (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
}

// Process affinity across all processor groups. Threads are spread over the groups round robin,
// because a Windows thread can belong to one group only.
bool ProcessSupervisor::SetAffinityAllGroups(DWORD pid) {
    WORD groupCount = (WORD)GetActiveProcessorGroupCount();
    if (!groupCount) return false;
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    // A process belongs to one group; its threads are spread over all groups below.
    bool ok = false;
    std::vector<DWORD> tids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        if (Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID == pid) tids.push_back(te.th32ThreadID);
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
    }
    for (size_t i = 0; i < tids.size(); ++i) {
        WORD g = (WORD)(i % groupCount);
        DWORD count = GetActiveProcessorCount(g);
        GROUP_AFFINITY ga{};
        ga.Group = g;
        ga.Mask = count >= 64 ? ~(KAFFINITY)0 : (((KAFFINITY)1 << count) - 1);
        HANDLE th = OpenThread(THREAD_SET_INFORMATION, FALSE, tids[i]);
        if (th) {
            if (SetThreadGroupAffinity(th, &ga, nullptr)) ok = true;
            CloseHandle(th);
        }
    }
    CloseHandle(h);
    return ok;
}

// ===================== Single threads =====================
static ULONGLONG FtToU64(const FILETIME& f) {
    return ((ULONGLONG)f.dwHighDateTime << 32) | (ULONGLONG)f.dwLowDateTime;
}

std::vector<ThreadInfo> ProcessSupervisor::EnumerateThreads(DWORD pid) {
    std::vector<ThreadInfo> out;
    ULONGLONG now = GetTickCount64();
    double elapsedMs = (double)(now - m_thTickMs);
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    double cores = si.dwNumberOfProcessors ? (double)si.dwNumberOfProcessors : 1.0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    std::map<DWORD, ULONGLONG> fresh;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            ThreadInfo ti;
            ti.tid = te.th32ThreadID;
            HANDLE th = OpenThread(THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE, ti.tid);
            ULONGLONG cpu = 0;
            if (th) {
                FILETIME c{}, e{}, k{}, u{};
                if (GetThreadTimes(th, &c, &e, &k, &u)) cpu = FtToU64(k) + FtToU64(u);
                ti.priority = GetThreadPriority(th);
                CloseHandle(th);
            }
            ti.cpuTimeMs = cpu / 10000;
            auto prev = m_thTimes.find(ti.tid);
            if (prev != m_thTimes.end() && elapsedMs >= 200.0 && cpu >= prev->second)
                ti.cpuPercent = 100.0 * ((double)(cpu - prev->second) / 10000.0) / (elapsedMs * cores);
            fresh[ti.tid] = cpu;
            out.push_back(ti);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    if (elapsedMs >= 200.0 || m_thTickMs == 0) {
        for (const auto& kv : fresh) m_thTimes[kv.first] = kv.second;
        m_thTickMs = now;
    }
    return out;
}
bool ProcessSupervisor::SetThreadPriorityLevel(DWORD tid, int level) {
    HANDLE th = OpenThread(THREAD_SET_INFORMATION, FALSE, tid);
    if (!th) return false;
    BOOL ok = SetThreadPriority(th, level);
    CloseHandle(th);
    return ok != FALSE;
}
bool ProcessSupervisor::SetThreadAffinityOf(DWORD tid, DWORD_PTR mask) {
    HANDLE th = OpenThread(THREAD_SET_INFORMATION, FALSE, tid);
    if (!th) return false;
    DWORD_PTR r = SetThreadAffinityMask(th, mask);
    CloseHandle(th);
    return r != 0;
}
bool ProcessSupervisor::SetThreadBoostOf(DWORD tid, bool on) {
    HANDLE th = OpenThread(THREAD_SET_INFORMATION, FALSE, tid);
    if (!th) return false;
    BOOL ok = SetThreadPriorityBoost(th, !on);
    CloseHandle(th);
    return ok != FALSE;
}
bool ProcessSupervisor::SetThreadSuspendedOf(DWORD tid, bool suspend) {
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME, FALSE, tid);
    if (!th) return false;
    DWORD r = suspend ? SuspendThread(th) : ResumeThread(th);
    CloseHandle(th);
    return r != (DWORD)-1;
}
