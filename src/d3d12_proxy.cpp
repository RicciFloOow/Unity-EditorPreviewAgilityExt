// ============================================================================
// UnityEditorPreviewAgilityExt - d3d12.dll proxy
//
// 目的：让 Unity 编辑器（其 Unity.exe 已导出 D3D12SDKVersion / D3D12SDKPath）
//       在不修改任何磁盘文件的前提下，按需切到 preview 分发的 D3D12Core。
//
// 部署规则（约定，不可配置）：
//   <Editor>\d3d12.dll        本代理（名字必须叫 d3d12.dll，与 Unity.exe 同级）
//   <Editor>\agility.uedtcfg  行为配置（不存在 = 完全纯转发，与原厂等价）
//   <Editor>\D3D12Pre\        preview 分发目录，内含同版本的
//                             D3D12Core.dll + d3d12SDKLayers.dll
//   <Editor>\D3D12\           Unity 原厂目录，永不触碰
//
// 激活优先级：
//   1. 命令行包含 -d3d12pre（按项目启动，Hub 可序列化参数）
//   2. agility.uedtcfg 里 [agility] enabled = 1
//   3. 以上都没有 -> 纯转发（默认行为）
//
// 版本号不写死：从 D3D12Pre\D3D12Core.dll 自身导出的 D3D12SDKVersion 探测。
//
// 补丁成功时会自动对 preview core 调一次
// D3D12EnableExperimentalFeatures(D3D12ExperimentalShaderModels)，
// 否则 SM 6.x preview（如 6.10）的 PSO 创建会被运行时拒绝；
// 该调用发生在任何设备创建之前，满足 D3D12 的时序约束。
//
// 配置示例：
//   [agility]
//   enabled = 1
//   strict  = 0    ; 1 = 分发异常时打"必失败"补丁，让设备创建报 0x887e0003 暴露问题
//
// 注意：DllMain 里做 LoadLibrary/读文件超出了严格的 loader-lock 规范，
//       但这是 ASI-loader 类代理的标准做法；Unity 在图形初始化时才加载
//       d3d12，此时进程已完全就绪，实践中无风险。
// ============================================================================

#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>

// ---------------------------------------------------------------------------
// 规则常量（约定 > 配置，一律不进 uedtcfg）
// ---------------------------------------------------------------------------
static constexpr wchar_t kConfigFileName[] = L"agility.uedtcfg";
static constexpr wchar_t kPreviewDirName[] = L"D3D12Pre";
static constexpr wchar_t kCliSwitch[]      = L"-d3d12pre";
static constexpr wchar_t kRealCoreName[]   = L"D3D12Core.dll";
static constexpr wchar_t kRealLayersName[] = L"d3d12SDKLayers.dll";

// 写入 exe 导出槽的相对路径（运行时按进程 exe 目录解析）。
// 静态存储，生命周期与代理一致。
static char g_sdkPathRel[] = ".\\D3D12Pre\\";

// ---------------------------------------------------------------------------
// 转发表：顺序必须与 thunks.asm / kExportNames 完全一致
// ---------------------------------------------------------------------------
#include "generated_names.h"
#include "generated_pragmas.h"

extern "C" UINT_PTR g_procs[AGILITY_EXPORT_COUNT] = {};

// ---------------------------------------------------------------------------
// 日志（追加写，一行一条）
// ---------------------------------------------------------------------------
static std::wstring g_proxyDir;
static std::wstring g_exeDir;

static void Log(const wchar_t* fmt, ...)
{
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    wchar_t line[1200];
    _snwprintf_s(line, _TRUNCATE, L"[d3d12proxy] %s\n", buf);
    OutputDebugStringW(line);
}

// ---------------------------------------------------------------------------
// 路径工具
// ---------------------------------------------------------------------------
static std::wstring DirOfModule(HMODULE mod)
{
    wchar_t path[MAX_PATH * 4];
    DWORD len = GetModuleFileNameW(mod, path, (DWORD)std::size(path));
    if (len == 0 || len >= std::size(path)) return {};
    std::wstring s(path, len);
    size_t pos = s.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? s : s.substr(0, pos);
}

static bool EqualsNoCase(const std::wstring& a, const std::wstring& b)
{
    return a.size() == b.size() && CompareStringOrdinal(a.c_str(), (int)a.size(),
                                                        b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

// ---------------------------------------------------------------------------
// PE 工具：导出表查找
// ---------------------------------------------------------------------------
struct PeSectionsView {
    const BYTE* base;             // 内存镜像基址，或文件字节缓冲基址
    bool isMapped;                // true=已映射镜像(RVA即指针) false=原始文件字节(需经节表换算)
    PIMAGE_SECTION_HEADER secs;
    WORD secCount;
};

static BYTE* RvaToRaw(const PeSectionsView& v, DWORD rva)
{
    if (v.isMapped) return const_cast<BYTE*>(v.base) + rva;
    for (WORD i = 0; i < v.secCount; ++i) {
        DWORD va = v.secs[i].VirtualAddress;
        DWORD span = max(v.secs[i].Misc.VirtualSize, v.secs[i].SizeOfRawData);
        if (rva >= va && rva < va + span)
            return const_cast<BYTE*>(v.base) + v.secs[i].PointerToRawData + (rva - va);
    }
    return nullptr;
}

static bool MakeView(const BYTE* base, bool isMapped, PeSectionsView& out, DWORD& exportDirRva)
{
    if (base[0] != 'M' || base[1] != 'Z') return false;
    DWORD peOff = *reinterpret_cast<const DWORD*>(base + 0x3C);
    const BYTE* nt = base + peOff;
    if (*reinterpret_cast<const DWORD*>(nt) != IMAGE_NT_SIGNATURE) return false;
    auto* fh = reinterpret_cast<const IMAGE_FILE_HEADER*>(nt + 4);
    const BYTE* opt = nt + 4 + sizeof(IMAGE_FILE_HEADER);
    WORD magic = *reinterpret_cast<const WORD*>(opt);
    DWORD ddBase = (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) ? 112u : 96u;
    exportDirRva = *reinterpret_cast<const DWORD*>(opt + ddBase); // dir[0] = export
    out.base = base;
    out.isMapped = isMapped;
    out.secs = reinterpret_cast<PIMAGE_SECTION_HEADER>(const_cast<BYTE*>(opt) + fh->SizeOfOptionalHeader);
    out.secCount = fh->NumberOfSections;
    return true;
}

// 在导出表中按名字找 RVA（data 或 code 导出均可）
static bool FindExportRva(const PeSectionsView& v, DWORD exportDirRva, const char* name, DWORD& outRva)
{
    if (!exportDirRva) return false;
    auto* dir = reinterpret_cast<PIMAGE_EXPORT_DIRECTORY>(RvaToRaw(v, exportDirRva));
    if (!dir) return false;
    auto* names = reinterpret_cast<DWORD*>(RvaToRaw(v, dir->AddressOfNames));
    auto* ords  = reinterpret_cast<WORD*>(RvaToRaw(v, dir->AddressOfNameOrdinals));
    auto* funcs = reinterpret_cast<DWORD*>(RvaToRaw(v, dir->AddressOfFunctions));
    if (!names || !ords || !funcs) return false;
    for (DWORD i = 0; i < dir->NumberOfNames; ++i) {
        const char* s = reinterpret_cast<const char*>(RvaToRaw(v, names[i]));
        if (s && strcmp(s, name) == 0) {
            outRva = funcs[ords[i]];
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 版本探测：只读解析 D3D12Core.dll / d3d12SDKLayers.dll 文件字节，
// 读其导出的 D3D12SDKVersion 常量。不加载、不执行目标 DLL 任何代码。
// ---------------------------------------------------------------------------
static bool ProbeSdkVersion(const std::wstring& dllPath, UINT& outVersion)
{
    HANDLE h = CreateFileW(dllPath.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(h, nullptr);
    if (size == INVALID_FILE_SIZE || size > 64 * 1024 * 1024) { CloseHandle(h); return false; }
    auto* buf = new (std::nothrow) BYTE[size];
    if (!buf) { CloseHandle(h); return false; }
    DWORD rd = 0;
    BOOL ok = ReadFile(h, buf, size, &rd, nullptr);
    CloseHandle(h);
    if (!ok || rd != size) { delete[] buf; return false; }

    bool result = false;
    PeSectionsView v{};
    DWORD exportDirRva = 0, symRva = 0;
    if (MakeView(buf, false, v, exportDirRva) && FindExportRva(v, exportDirRva, "D3D12SDKVersion", symRva)) {
        BYTE* p = RvaToRaw(v, symRva);
        if (p) { outVersion = *reinterpret_cast<UINT*>(p); result = true; }
    }
    delete[] buf;
    return result;
}

// ---------------------------------------------------------------------------
// 内存补丁：改写进程 exe 的 D3D12SDKVersion / D3D12SDKPath 导出数据
// ---------------------------------------------------------------------------
static bool PatchExeExports(UINT version, const char* pathRel, std::wstring& err)
{
    BYTE* exe = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    if (!exe) { err = L"GetModuleHandle(NULL) failed"; return false; }

    PeSectionsView v{};
    DWORD exportDirRva = 0;
    if (!MakeView(exe, true, v, exportDirRva)) { err = L"exe is not a valid PE"; return false; }

    DWORD verRva = 0, pathRva = 0;
    if (!FindExportRva(v, exportDirRva, "D3D12SDKVersion", verRva)) {
        err = L"exe has no D3D12SDKVersion export (not an Agility-enabled Unity?)"; return false;
    }
    if (!FindExportRva(v, exportDirRva, "D3D12SDKPath", pathRva)) {
        err = L"exe has no D3D12SDKPath export"; return false;
    }

    DWORD imageSize = 0;
    {
        DWORD peOff = *reinterpret_cast<DWORD*>(exe + 0x3C);
        // SizeOfImage 位于 OptionalHeader 偏移 56
        imageSize = *reinterpret_cast<DWORD*>(exe + peOff + 4 + sizeof(IMAGE_FILE_HEADER) + 56);
    }

    // 1) 版本号：一个 UINT 常量槽
    {
        UINT* slot = reinterpret_cast<UINT*>(exe + verRva);
        UINT old = *slot;
        DWORD oldProtect;
        if (!VirtualProtect(slot, sizeof(UINT), PAGE_READWRITE, &oldProtect)) { err = L"VirtualProtect(version) failed"; return false; }
        *slot = version;
        DWORD tmp; VirtualProtect(slot, sizeof(UINT), old, &tmp);
        Log(L"patched D3D12SDKVersion: %u -> %u (rva=0x%lx)", old, version, verRva);
    }

    // 2) 路径：两种形态都兼容
    //    a) 指针槽（const char* 变量，内容是指向 exe 镜像内字符串的地址）——实测 Unity 6000.6 是这种
    //    b) 内联字符串槽（槽内直接是 ".\D3D12\" 字面量）
    {
        BYTE* slot = exe + pathRva;
        UINT_PTR val = *reinterpret_cast<UINT_PTR*>(slot);
        bool isPointerSlot =
            val >= reinterpret_cast<UINT_PTR>(exe) &&
            val <  reinterpret_cast<UINT_PTR>(exe) + imageSize &&
            reinterpret_cast<const char*>(val)[0] == '.';

        DWORD oldProtect;
        if (isPointerSlot) {
            if (!VirtualProtect(slot, sizeof(UINT_PTR), PAGE_READWRITE, &oldProtect)) { err = L"VirtualProtect(path ptr) failed"; return false; }
            *reinterpret_cast<const char**>(slot) = pathRel;
            DWORD tmp; VirtualProtect(slot, sizeof(UINT_PTR), oldProtect, &tmp);
            Log(L"patched D3D12SDKPath: pointer slot -> \"%hs\"", pathRel);
        } else if (slot[0] == '.' || slot[0] == '\\') {
            size_t need = strlen(pathRel) + 1;
            if (need > 64) { err = L"inline path slot too small"; return false; }
            if (!VirtualProtect(slot, 64, PAGE_READWRITE, &oldProtect)) { err = L"VirtualProtect(path str) failed"; return false; }
            memset(slot, 0, 64);
            memcpy(slot, pathRel, need);
            DWORD tmp; VirtualProtect(slot, 64, oldProtect, &tmp);
            Log(L"patched D3D12SDKPath: inline slot -> \"%hs\"", pathRel);
        } else {
            err = L"unrecognized D3D12SDKPath slot layout";
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// preview shader model 放行：开启实验性 shader model。
// SM 6.x preview（如 6.10）未经此调用，PSO 创建会被运行时拒绝。
//
// 注意调用对象：D3D12EnableExperimentalFeatures 是 d3d12.dll **前端**的导出，
// D3D12Core.dll 并不导出它。补丁已把 exe 的 D3D12SDKVersion/Path 指向 preview
// 分发，因此调前端的该 API 会按同一条 Agility 转发链落到 preview core 上——
// 与设备创建走的是同一条路。
// 时序约束：D3D12 规定变更特性开关会使已存在设备 DEVICE_REMOVED，
// 因此必须早于任何设备创建——ProxyInit 阶段进程内必然还没有设备，天然安全。
// 只在已成功补丁到 preview 分发后调用；纯转发/回落分支不调
// （对 retail core 调用只会得到 E_NOINTERFACE）。
// ---------------------------------------------------------------------------
static void EnableExperimentalShaderModels(HMODULE hRealD3D12)
{
    // D3D12ExperimentalShaderModels（见 d3d12.h）：无需配置结构体
    static const GUID kExpShaderModels = { 0x76f5573e, 0xf13a, 0x40f5,
        { 0xb2, 0x97, 0x81, 0xce, 0x9e, 0x18, 0x93, 0x3f } };

    typedef HRESULT (WINAPI *EnableExpFn)(UINT, const IID*, void*, UINT*);
    auto pfn = reinterpret_cast<EnableExpFn>(
        GetProcAddress(hRealD3D12, "D3D12EnableExperimentalFeatures"));
    if (!pfn) { Log(L"enableExpSM: D3D12EnableExperimentalFeatures not found on frontend"); return; }

    HRESULT hr = pfn(1, &kExpShaderModels, nullptr, nullptr);
    Log(L"enableExpSM: D3D12EnableExperimentalFeatures -> 0x%08lx", (unsigned long)hr);
}

// ---------------------------------------------------------------------------
// 配置解析：INI-lite，只认 [agility] 下的 enabled / strict
// ---------------------------------------------------------------------------
struct Config { bool fileFound = false; bool enabled = false; bool strict = false; };

static Config ParseConfig(const std::wstring& path)
{
    Config cfg{};
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return cfg;
    cfg.fileFound = true;
    char buf[4096]; DWORD rd = 0;
    BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &rd, nullptr);
    CloseHandle(h);
    if (!ok) return cfg;
    buf[rd] = '\0';

    bool inSection = false;
    for (char* p = buf; *p; ) {
        char* eol = strpbrk(p, "\r\n");
        if (eol) *eol = '\0';
        // 去首尾空白
        while (*p == ' ' || *p == '\t') ++p;
        char* end = p + strlen(p);
        while (end > p && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';

        if (*p == '[') {
            inSection = (_stricmp(p, "[agility]") == 0);
        } else if (inSection && *p && *p != ';' && *p != '#') {
            char* eq = strchr(p, '=');
            if (eq) {
                *eq = '\0';
                // 简易 trim
                auto trim = [](char* s) -> char* {
                    while (*s == ' ' || *s == '\t') ++s;
                    char* e = s + strlen(s);
                    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) *--e = '\0';
                    return s;
                };
                char* key = trim(p); char* value = trim(eq + 1);
                bool truthy = (_stricmp(value, "1") == 0 || _stricmp(value, "true") == 0 ||
                               _stricmp(value, "yes") == 0 || _stricmp(value, "on") == 0);
                if (_stricmp(key, "enabled") == 0) cfg.enabled = truthy;
                else if (_stricmp(key, "strict") == 0) cfg.strict = truthy;
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
    return cfg;
}

// ---------------------------------------------------------------------------
// 命令行开关：-d3d12pre（大小写不敏感，要求 token 边界）
// ---------------------------------------------------------------------------
static bool HasCliSwitch()
{
    const wchar_t* cmd = GetCommandLineW();
    if (!cmd) return false;
    const size_t swLen = wcslen(kCliSwitch);
    for (const wchar_t* p = cmd; *p; ++p) {
        if ((*p == L'-') && _wcsnicmp(p, kCliSwitch, swLen) == 0) {
            wchar_t next = p[swLen];
            if (next == L'\0' || next == L' ' || next == L'\t' || next == L'"')
                return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 加载真身：System32 路径由系统 API 组合，绝不写死盘符
// ---------------------------------------------------------------------------
static HMODULE LoadRealD3D12()
{
    wchar_t sysDir[MAX_PATH];
    UINT n = GetSystemDirectoryW(sysDir, (UINT)std::size(sysDir)); // 64位进程无WOW64重定向
    if (n == 0 || n >= std::size(sysDir)) { Log(L"GetSystemDirectoryW failed"); return nullptr; }

    std::wstring realPath = std::wstring(sysDir) + L"\\d3d12.dll";
    HMODULE h = LoadLibraryExW(realPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!h) { Log(L"LoadLibraryExW(%s) failed, gle=%lu", realPath.c_str(), GetLastError()); return nullptr; }

    // 自检：返回的绝不能是代理自己（同名模块短路是所有代理的共同陷阱）
    std::wstring loadedPath = DirOfModule(h);
    if (EqualsNoCase(loadedPath, g_proxyDir)) {
        Log(L"FATAL: real d3d12 resolved to proxy dir, refusing to forward");
        FreeLibrary(h);
        return nullptr;
    }
    return h;
}

static void FillProcTable(HMODULE hReal)
{
    for (size_t i = 0; i < AGILITY_EXPORT_COUNT; ++i) {
        g_procs[i] = reinterpret_cast<UINT_PTR>(GetProcAddress(hReal, kExportNames[i]));
        if (!g_procs[i]) Log(L"WARN: GetProcAddress(%hs) failed on real d3d12.dll", kExportNames[i]);
    }
}

// ---------------------------------------------------------------------------
// 初始化主流程
// ---------------------------------------------------------------------------
static void ProxyInit(HMODULE hSelf)
{
    g_proxyDir = DirOfModule(hSelf);
    g_exeDir   = DirOfModule(nullptr);

    Log(L"proxy loaded. proxyDir=%s exeDir=%s", g_proxyDir.c_str(), g_exeDir.c_str());

    // 激活判定：命令行 > 配置 > 默认
    bool forceCli = HasCliSwitch();
    Config cfg = ParseConfig(g_proxyDir + L"\\" + kConfigFileName);
    bool active = forceCli || cfg.enabled;

    Log(L"activation: cli=%d configFound=%d enabled=%d strict=%d -> active=%d",
        forceCli, cfg.fileFound, cfg.enabled, cfg.strict, active);

    // 补丁阶段（仅激活时）；patched 仅在为 preview 分发补丁成功时置位。
    // 注意：此处起不允许 early return——转发表填充在函数末尾，任何分支都要走到。
    bool patched = false;
    if (active) {
    // 部署自检：代理必须与进程 exe 同目录，否则相对路径会被解析到错误位置
    if (!EqualsNoCase(g_proxyDir, g_exeDir)) {
        Log(L"FATAL: proxy dir != exe dir, refusing to patch (misdeployed proxy)");
    } else {

    // 版本探测：preview 分发双 DLL 的版本必须一致
    std::wstring preDir = g_proxyDir + L"\\" + kPreviewDirName + L"\\";
    UINT coreVer = 0, layersVer = 0;
    bool coreOk   = ProbeSdkVersion(preDir + kRealCoreName, coreVer);
    bool layersOk = ProbeSdkVersion(preDir + kRealLayersName, layersVer);
    bool probeOk  = coreOk && layersOk && (coreVer == layersVer);

    Log(L"probe: core=%u(%d) layers=%u(%d) -> %s",
        coreVer, coreOk, layersVer, layersOk, probeOk ? L"OK" : L"MISMATCH/MISSING");

    if (probeOk) {
        std::wstring err;
        if (PatchExeExports(coreVer, g_sdkPathRel, err)) {
            Log(L"preview distribution activated: version=%u path=%hs", coreVer, g_sdkPathRel);
            // preview SM（6.10）必须显式放行，否则 PSO 创建被运行时拒绝。
            // 调前端（System32 d3d12.dll）的该导出，由 Agility 转发链落到 preview core。
            patched = true;
        } else {
            Log(L"patch failed: %s (falling back to stock)", err.c_str());
        }
    } else if (cfg.strict) {
        // 暴露配置错误：版本写成必然不匹配的 0xFFFFFFFF，让设备创建报 0x887e0003
        std::wstring err;
        PatchExeExports(0xFFFFFFFFu, g_sdkPathRel, err);
        Log(L"strict mode: applied intentionally-breaking patch to surface misconfiguration");
    } else {
        Log(L"falling back to stock distribution (strict=0)");
    }
    }
    }
    // else 纯转发，与原厂行为等价

    // 前端加载与转发表填充：任何分支都必须执行（thunk 依赖 g_procs），
    // 且必须在补丁之后。System32 d3d12.dll（前端）加载时会解析并缓存 exe 的
    // D3D12SDKVersion/Path 导出；若先加载前端再打补丁，缓存的就是未补丁的
    // 618 + .\D3D12\，此后设备创建与 D3D12EnableExperimentalFeatures
    // 全部落在 retail core 上（enableExpSM -> E_NOINTERFACE，preview SM
    // 的 PSO -> E_INVALIDARG）。
    HMODULE hReal = LoadRealD3D12();
    if (hReal) FillProcTable(hReal);

    // preview SM（6.10）必须显式放行，否则 PSO 创建会被运行时拒绝。
    // 调前端（System32 d3d12.dll）的该导出，由 Agility 转发链落到 preview core。
    if (patched) {
        if (hReal) EnableExperimentalShaderModels(hReal);
        else Log(L"enableExpSM skipped: real d3d12 unavailable");
    }
}

// ---------------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        ProxyInit(hModule);
    }
    return TRUE;
}
