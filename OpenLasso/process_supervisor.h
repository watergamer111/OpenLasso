// process_supervisor.h - OpenLasso engine interface.
#ifndef PROCESS_SUPERVISOR_H
#define PROCESS_SUPERVISOR_H

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <objbase.h>
#include <tlhelp32.h>
#include <powrprof.h>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

// Columns shown by default (all except I/O priority and GPU memory).
static const DWORD kDefaultColumns = 0xAFFF;

struct PriorityEntry {
    DWORD value;
    const wchar_t* cfgName;
    const wchar_t* label;
};
extern const PriorityEntry kPriorities[];
extern const int kPriorityCount;
std::wstring PriorityToLabel(DWORD value);
extern const wchar_t* kIoLabels[4];

struct ProcessInfo {
    DWORD pid = 0;
    std::wstring name;
    DWORD priorityClass = 0;
    DWORD_PTR affinityMask = 0;
    DWORD memoryPriority = 0;
    bool ecoQos = false;
    double cpuPercent = 0.0;
    ULONGLONG cpuTimeMs = 0;
    DWORD parentPid = 0;
    DWORD threads = 0;
    std::wstring user;
    std::wstring path;
    DWORD ioPriority = 99;               // 0..3, or 99 when unknown
    double gpuPercent = 0.0;             // busiest GPU engine used by the process (PDH "GPU Engine")
    ULONGLONG gpuMemBytes = 0;           // dedicated GPU memory used by the process
    ULONGLONG workingSetKB = 0;          // RAM (working set)
    std::wstring cmdLine;                // full command line; filled on demand (CommandLineOf)
};

// priorityClass 0 = unchanged; affinityMask 0 = unchanged; memoryPriority 0 = unchanged
// eco: 0 unchanged, 1 on, 2 off; ioPriority: 0 unchanged, otherwise value+1
// threadBoost: 0 unchanged, 1 on, 2 off; powerPlan "" = none
struct ProcessRule {
    std::wstring processName;
    DWORD priorityClass = 0;
    DWORD_PTR affinityMask = 0;
    DWORD memoryPriority = 0;
    int eco = 0;
    DWORD ioPriority = 0;
    int threadBoost = 0;
    int cpuSets = 0;          // 0 unchanged, 1 all, 2 first half, 3 second half
    int gpuPriority = 0;      // 0 unchanged, else D3DKMT class + 1 (0 idle .. 5 realtime)
    int gpuPref = 0;          // 0 unchanged, 1 default, 2 power saving, 3 high performance
    bool preventSleep = false;
    bool forcePower = false;
    std::wstring powerPlan;
    bool keepRunning = false;             // restart the process when it exits
    bool keepRunningElevate = false;      // restart it as administrator
    std::wstring keepRunningCommandLine;  // arguments used for the restart ("" = those captured while it ran)
    std::wstring keepRunningPath;         // full path of the executable to start
    int limitPercent = 0;                 // CPU cap: pause the process above this % (0 = off)
    int guardCpu = 0;                        // CPU guard: CPU % threshold
    int guardSeconds = 0;                    // CPU guard: seconds above the threshold
    int guardAction = 0;                     // CPU guard: 0 off, 1 lower priority, 2 close, 3 run command
    std::wstring guardCommand;               // CPU guard: command for action 3
    bool block = false;                // close the process as soon as it starts
    bool balanceInstances = false;        // spread several instances over different CPUs
    bool enabled = true;
};

// One structured log record (shown as columns and written as one CSV row).
struct LogEntry {
    std::wstring time, computer, action, process, path, command;
    DWORD pid = 0;
};

enum class GpuResult { Applied, Unsupported, Denied, Ignored, Failed };

struct PowerPlanInfo {
    std::wstring guid;
    std::wstring name;
};

struct Settings {
    bool throttle = true;
    int loadThreshold = 80;
    int procThreshold = 10;
    bool boost = false;
    bool autoApply = true;
    bool forced = false;
    bool memoryOptimizer = false;
    bool idleSaver = false;
    int idleMinutes = 5;
    bool performance = false;
    bool manageAllUsers = true;
    bool closeToTray = true;
    bool startMinimized = true;
    int trayMode = 1;                    // 0 always, 1 when hidden or minimized, 2 never (while visible)
    bool logEnabled = true;
    bool smooth = true;
    bool showEvents = true;
    bool showCpuMeter = true;
    bool showRamMeter = true;
    bool showPlan = true;
    bool showCpuLine = true;
    bool treeView = true;
    bool gridlines = false;
    bool icons = true;
    bool tooltips = true;
    bool alwaysOnTop = false;
    bool darkTheme = false;
    bool cpuUtility = true;
    bool pauseOnRightClick = true;
    int throttleSeconds = 2;           // seconds above the threshold before auto-throttle lowers a process
    bool notifications = false;           // tray balloons for Throttle, CPU guard, block
    bool delegateToService = true;       // when the service runs, the window leaves rule application to it
    bool showRamLine = true;             // graph: RAM history line
    bool showGpuLine = true;             // graph: GPU history line
    bool showGpuMeter = true;
    bool gpuFallback = true;             // GPU priority ignored by the driver -> GPU preference
    bool customPerfPlan = true;          // High-power mode uses the OpenLasso High Power plan
    int maxLogKB = 1024;                 // log file size before it is rotated
    DWORD columnsMask = kDefaultColumns;
    int updateCheck = 1;                 // 0 never, 1 daily, 2 weekly
    bool includeBetas = false;
    std::wstring updateRepo;             // "owner/repo" used by Check for updates
    std::wstring logFolder;              // empty = folder of the exe
    std::vector<std::wstring> exclusions;
};

// Rule matching: name, full path, folder (trailing backslash), wildcards, default rule "*".
bool MatchGlob(const std::wstring& pattern, const std::wstring& text);
bool RuleMatchesProcess(const ProcessRule& r, const ProcessInfo& p, const std::vector<ProcessRule>& all);
// Command line (--version, --status, --list, --apply, --profile, --help). handled = true when used.
int RunCommandLine(const std::wstring& args, bool& handled);

// One thread of a process (Threads tab).
struct ThreadInfo {
    DWORD tid = 0;
    int priority = 0;
    ULONGLONG cpuTimeMs = 0;
    double cpuPercent = 0.0;
};

// Smoothness index: background probe thread measuring scheduler wake-up latency (0..100).
void StartSmoothness();
void StopSmoothness();
int SmoothnessIndex();

class ProcessSupervisor {
public:
    ProcessSupervisor();
    ~ProcessSupervisor();

    // ----- processes -----
    std::vector<ProcessInfo> EnumerateProcesses();
    void SampleSystemLoad();
    double SystemLoad() const { return m_systemLoad; }
    double MemoryLoad() const { return m_memLoad; }
    double MemoryTotalGB() const { return m_memTotalGB; }
    double MemoryUsedGB() const { return m_memUsedGB; }

    bool SetPriorityClass(DWORD pid, DWORD priority);
    bool SetAffinityMask(DWORD pid, DWORD_PTR mask);
    bool SetMemoryPriority(DWORD pid, DWORD prio);
    bool SetEcoQos(DWORD pid, bool on);
    bool SetIoPriority(DWORD pid, DWORD prio);
    bool SetThreadBoost(DWORD pid, bool on);
    bool FreeProcessMemory(DWORD pid);
    bool CloseProcessWindows(DWORD pid);
    bool EndProcess(DWORD pid);
    bool RestartProcess(const ProcessInfo& p, bool asAdmin = false);   // keeps the original command line
    bool ApplyToProcess(DWORD pid, const ProcessRule& r, const std::wstring& path = L"",
                        const ProcessInfo* info = nullptr);
    const std::wstring& LastNote() const { return m_lastNote; }      // explanation of the last partial result
    std::wstring CommandLineOf(DWORD pid);                           // full command line (cached)
    static std::wstring StripArgv0(const std::wstring& cmd);         // arguments only
    static bool LaunchProcess(const std::wstring& path, const std::wstring& args, bool elevate);
    void NoteUserExit(const std::wstring& name);                     // do not "keep running" this exit
    void SetServiceMode(bool s) { m_serviceMode = s; }
    bool SetCpuSets(DWORD pid, int mode);
    bool SetGpuPriority(DWORD pid, DWORD v);
    GpuResult SetGpuPriorityEx(DWORD pid, DWORD v);                   // checks WDDM and reads the value back
    static const wchar_t* GpuResultText(GpuResult r);
    static bool GpuSchedulingSupported();
    void SampleGpu();
    void SampleExtras();                                             // per-core CPU, disk and network (1 Hz)
    const std::vector<double>& CoreLoads() const { return m_coreLoad; }
    double DiskPercent() const { return m_diskPct; }
    double NetKBps() const { return m_netKBps; }
    void ServiceHeartbeat();                                         // service writes its status file
    bool ServiceRunning() const;                                     // window: is the service alive?
    void Notify(const std::wstring& text);                           // queued tray balloon
    std::vector<std::wstring> TakeNotes();
    void RecordStat(const std::wstring& name, double cpu);           // throttle CSV report
    std::wstring ThrottleReportPath() const;
    std::vector<ThreadInfo> EnumerateThreads(DWORD pid);
    bool SetThreadPriorityLevel(DWORD tid, int level);
    bool SetThreadAffinityOf(DWORD tid, DWORD_PTR mask);
    bool SetThreadBoostOf(DWORD tid, bool on);
    bool SetThreadSuspendedOf(DWORD tid, bool suspend);
    bool SetAffinityAllGroups(DWORD pid);                            // all CPUs of all processor groups
    int LogicalProcessorCount();                                                // GPU engines + GPU memory (PDH), ~1 Hz
    double GpuLoad() const { return m_gpuLoad; }
    bool GpuAvailable() const { return m_gpuAvail; }
    double GpuMemUsedMB() const { return m_gpuMemTotal / (1024.0 * 1024.0); }
    bool SetGpuPreference(const std::wstring& exe, int regValue);   // 0 default, 1 power saving, 2 high performance
    void SetSleepHold(DWORD pid, bool on);
    void SetPerfHold(DWORD pid, bool on);
    bool SleepHeld(DWORD pid) const { return m_sleepPids.count(pid) != 0; }
    bool PerfHeld(DWORD pid) const { return m_perfPids.count(pid) != 0; }
    int FreeBackgroundMemory(const std::vector<ProcessInfo>& procs);

    // ----- engine -----
    int ApplyAll(const std::vector<ProcessInfo>& procs);
    void SetEngineStopped(bool s) { m_stopped = s; }
    bool EngineStopped() const { return m_stopped; }
    void SetKeepAwake(int minutes);          // 0 off, -1 indefinitely
    void TickKeepAwake();
    bool KeepAwakeActive() const { return m_awakeUntil != 0; }

    // ----- power plans -----
    static std::vector<PowerPlanInfo> ListPowerPlans();
    static bool SetActivePowerPlan(const std::wstring& guid);
    static bool GetActivePowerPlan(std::wstring& guid);
    // Own plan, duplicated from High performance: CPU 100%, boost, no core parking, no USB/PCIe/disk saving.
    bool EnsurePerformancePlan(std::wstring& guid, bool rewrite = false);
    bool PerformancePlanGuid(std::wstring& guid);
    static const wchar_t* PerfPlanName();

    // ----- configuration -----
    std::vector<ProcessRule>& Rules() { return m_rules; }
    Settings& Cfg() { return m_cfg; }
    const std::wstring& ConfigPath() const { return m_configPath; }
    std::wstring ExeDir() const;
    bool LoadConfig();
    bool SaveConfig();
    bool LoadConfigFrom(const std::wstring& path);
    bool SaveConfigTo(const std::wstring& path);
    bool LoadRulesFrom(const std::wstring& path);
    bool SaveRulesTo(const std::wstring& path);
    void ResetConfig();
    static std::string RulesToJson(const std::vector<ProcessRule>& rules);
    static std::vector<ProcessRule> RulesFromJson(const std::string& text);
    static std::vector<std::wstring> ListProfiles(const std::wstring& dir);
    std::wstring ProfilePath(const std::wstring& name) const;

    // ----- log -----
    const std::deque<LogEntry>& Log() const { return m_log; }
    void ClearLog() { m_log.clear(); ++m_logSeq; }
    void AddLog(const std::wstring& msg, const ProcessInfo* p = nullptr);
    ULONGLONG LogSeq() const { return m_logSeq; }
    static std::wstring LogEntryText(const LogEntry& e);
    std::wstring LogFilePath() const;

    // ----- Throttle / boost state -----
    bool IsLowered(DWORD pid) const { return m_lowered.count(pid) != 0; }
    bool IsBoosted(DWORD pid) const { return m_boosted.count(pid) != 0; }
    ULONGLONG LoweredSince(DWORD pid) const {
        auto it = m_lowerStart.find(pid);
        return it == m_lowerStart.end() ? 0 : it->second;
    }
    DWORD ThrottleTotal() const { return m_pbTotal; }
    DWORD ThrottleToday() const { return m_pbToday; }
    size_t LoweredCount() const { return m_lowered.size(); }
    std::wstring SummaryText() const;

    // ----- helpers -----
    bool IsRuleControlled(const std::wstring& name) const;
    bool IsExcluded(const std::wstring& name) const;

    // ----- start with Windows -----
    static bool GetAutostart();
    static bool SetAutostart(bool on, const std::wstring& exePath);

    // ----- Windows service -----
    bool InstallService();
    bool UninstallService();
    bool StartSvc();
    bool StopSvc();

private:
    void CountEvent();
    int RunFeatures(const std::vector<ProcessInfo>& procs);
    int RunBlock(const std::vector<ProcessInfo>& procs);
    int RunGuard(const std::vector<ProcessInfo>& procs);
    int RunCap(const std::vector<ProcessInfo>& procs);
    int RunSpread(const std::vector<ProcessInfo>& procs);
    void ResumeLimited();
    void SampleIo();
    void CloseIoPdh();
    std::map<DWORD, int> m_hot;
    std::map<DWORD, ULONGLONG> m_limitResume, m_wdSince;
    std::set<DWORD> m_wdFired;
    std::vector<std::wstring> m_notes;
    std::vector<double> m_coreLoad;
    std::vector<ULONGLONG> m_prevCoreIdle, m_prevCoreKern, m_prevCoreUser;
    double m_diskPct = 0.0, m_netKBps = 0.0;
    HANDLE m_ioQuery = nullptr, m_ioDisk = nullptr, m_ioNet = nullptr;
    ULONGLONG m_ioNext = 0;
    std::map<DWORD, ULONGLONG> m_thTimes;
    ULONGLONG m_thTickMs = 0;
    int RunRules(const std::vector<ProcessInfo>& procs);
    int RunThrottle(const std::vector<ProcessInfo>& procs, DWORD fg);
    int RunBoost(const std::vector<ProcessInfo>& procs, DWORD fg);
    int RunMemoryOptimizer(const std::vector<ProcessInfo>& procs, DWORD fg);
    int RunPowerPlan(const std::vector<ProcessInfo>& procs);
    int RunKeepRunning(const std::vector<ProcessInfo>& procs);
    GpuResult ApplyGpuRule(const ProcessInfo& p, const ProcessRule& r);
    void ConfigurePerfPlan(const GUID& plan);
    void ClosePdh();
    int RunHolds(const std::vector<ProcessInfo>& procs);
    std::set<DWORD> m_sleepPids, m_perfPids;
    std::map<DWORD, int> m_appliedCpu, m_appliedBoost, m_appliedPref;
    std::map<DWORD, int> m_appliedGpu;
    bool m_sleepHeldNow = false;
    bool m_perfNeeded = false;
    bool FindPlan(const char* keyword, std::wstring& guid);

    std::vector<ProcessRule> m_rules;
    Settings m_cfg;
    std::wstring m_configPath;
    std::deque<LogEntry> m_log;
    ULONGLONG m_logSeq = 0;
    std::wstring m_lastNote;
    bool m_serviceMode = false;
    std::map<DWORD, int> m_gpuLogged;
    std::map<DWORD, std::wstring> m_cmdCache;
    std::wstring m_perfPlanGuid;

    struct KeepState {
        bool seen = false, userEnded = false;
        ULONGLONG pausedUntil = 0;
        std::wstring path, args;
        std::vector<ULONGLONG> restarts;
    };
    std::map<std::wstring, KeepState> m_keep;

    // GPU counters (PDH)
    HANDLE m_pdhQuery = nullptr, m_pdhUtil = nullptr, m_pdhMem = nullptr;
    ULONGLONG m_pdhOpened = 0;
    bool m_pdhFailed = false, m_gpuAvail = false;
    double m_gpuLoad = 0.0;
    ULONGLONG m_gpuMemTotal = 0;
    std::map<DWORD, double> m_gpuPid;
    std::map<DWORD, ULONGLONG> m_gpuMem;
    bool m_stopped = false;
    ULONGLONG m_awakeUntil = 0;

    ULONGLONG m_prevTick = 0;
    std::map<DWORD, ULONGLONG> m_prevTimes;
    std::map<DWORD, double> m_lastCpu;
    bool m_sysValid = false;
    ULONGLONG m_prevIdle = 0, m_prevKern = 0, m_prevUser = 0;
    double m_systemLoad = 0.0;
    double m_memLoad = 0.0, m_memTotalGB = 0.0, m_memUsedGB = 0.0;

    std::map<DWORD, DWORD> m_lowered;
    std::map<DWORD, DWORD> m_boosted;
    std::map<DWORD, ULONGLONG> m_lowerStart;
    DWORD m_pbTotal = 0, m_pbToday = 0;
    WORD m_pbDay = 0;
    ULONGLONG m_lastFree = 0;

    bool m_planOverridden = false;
    std::wstring m_originalPlan;

    std::map<DWORD, std::wstring> m_userCache;
};

#endif // PROCESS_SUPERVISOR_H
