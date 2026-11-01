// ============================================================================
// gen_exports.exe - d3d12 代理导出表自同步生成器（构建期工具）
//
// 职责：读取本机 System32\d3d12.dll 的导出表（名称 + 序号），生成：
//   generated_pragmas.h   #pragma comment(linker, "/export:NAME,@ORD") 列表
//   generated_thunks.inc  thunks.asm 用的 THUNK 宏调用列表（INCLUDE）
//   generated_names.h     kExportNames[] 与 AGILITY_EXPORT_COUNT
//
// 可选参数：[Unity编辑器目录]  传入后额外校验该目录 Unity.dll 对 d3d12.dll
//   的全部导入（按名 + 按序号）均被代理导出覆盖，缺项则以退出码 2 失败。
//
// 零依赖：纯 WinAPI，cl 单文件直编。系统 d3d12 更新后重跑本工具即可再同步。
// ============================================================================

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct ExportEntry { std::string name; unsigned ordinal; };

static bool ReadFileAll(const wchar_t* path, std::vector<BYTE>& out)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(h, nullptr);
    if (size == INVALID_FILE_SIZE || size == 0 || size > 1024 * 1024 * 1024) { CloseHandle(h); return false; }
    out.resize(size);
    DWORD rd = 0;
    BOOL ok = ReadFile(h, out.data(), size, &rd, nullptr);
    CloseHandle(h);
    return ok && rd == size;
}

struct PeView {
    const BYTE* base;
    PIMAGE_SECTION_HEADER secs;
    WORD secCount;
    DWORD exportDirRva;
    DWORD importDirRva;
};

static BYTE* RvaToRaw(const PeView& v, DWORD rva)
{
    for (WORD i = 0; i < v.secCount; ++i) {
        DWORD va = v.secs[i].VirtualAddress;
        DWORD span = max(v.secs[i].Misc.VirtualSize, v.secs[i].SizeOfRawData);
        if (rva >= va && rva < va + span)
            return const_cast<BYTE*>(v.base) + v.secs[i].PointerToRawData + (rva - va);
    }
    return nullptr;
}

static bool MakePeView(const std::vector<BYTE>& img, PeView& v)
{
    const BYTE* b = img.data();
    if (img.size() < 0x1000 || b[0] != 'M' || b[1] != 'Z') return false;
    DWORD peOff = *reinterpret_cast<const DWORD*>(b + 0x3C);
    const BYTE* nt = b + peOff;
    if (*reinterpret_cast<const DWORD*>(nt) != IMAGE_NT_SIGNATURE) return false;
    auto* fh = reinterpret_cast<const IMAGE_FILE_HEADER*>(nt + 4);
    const BYTE* opt = nt + 4 + sizeof(IMAGE_FILE_HEADER);
    WORD magic = *reinterpret_cast<const WORD*>(opt);
    DWORD ddBase = (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) ? 112u : 96u;
    v.base = b;
    v.secs = reinterpret_cast<PIMAGE_SECTION_HEADER>(const_cast<BYTE*>(opt) + fh->SizeOfOptionalHeader);
    v.secCount = fh->NumberOfSections;
    v.exportDirRva = *reinterpret_cast<const DWORD*>(opt + ddBase);       // dir[0]
    v.importDirRva = *reinterpret_cast<const DWORD*>(opt + ddBase + 8);   // dir[1]
    return true;
}

static bool ParseExports(const PeView& v, std::vector<ExportEntry>& out, unsigned& ordBase)
{
    if (!v.exportDirRva) return false;
    auto* dir = reinterpret_cast<PIMAGE_EXPORT_DIRECTORY>(RvaToRaw(v, v.exportDirRva));
    if (!dir) return false;
    ordBase = dir->Base;
    auto* names = reinterpret_cast<DWORD*>(RvaToRaw(v, dir->AddressOfNames));
    auto* ords  = reinterpret_cast<WORD*>(RvaToRaw(v, dir->AddressOfNameOrdinals));
    if (!names || !ords) return false;
    for (DWORD i = 0; i < dir->NumberOfNames; ++i) {
        const char* s = reinterpret_cast<const char*>(RvaToRaw(v, names[i]));
        if (!s) return false;
        out.push_back({ s, ordBase + ords[i] });
    }
    return !out.empty();
}

// 校验 target（如 Unity.dll）对 d3d12.dll 的全部导入均被 exports 覆盖
static bool CheckImportsAgainst(const wchar_t* targetPath,
                                const std::vector<ExportEntry>& exports, unsigned ordBase)
{
    std::vector<BYTE> img;
    if (!ReadFileAll(targetPath, img)) {
        printf("[gen_exports] WARN: cannot read target, skip import check\n");
        return true; // 目标不存在不算失败（比如路径传错时只告警）
    }
    PeView v{};
    if (!MakePeView(img, v) || !v.importDirRva) { printf("[gen_exports] WARN: target has no imports\n"); return true; }

    auto* desc = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(RvaToRaw(v, v.importDirRva));
    if (!desc) return true;

    unsigned maxOrd = ordBase;
    for (auto& e : exports) if (e.ordinal > maxOrd) maxOrd = e.ordinal;

    bool ok = true;
    for (; desc->Name; ++desc) {
        const char* mod = reinterpret_cast<const char*>(RvaToRaw(v, desc->Name));
        if (!mod || _stricmp(mod, "d3d12.dll") != 0) continue;

        printf("[gen_exports] checking imports from d3d12.dll in target:\n");
        DWORD look = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
        auto* ilt = reinterpret_cast<PIMAGE_THUNK_DATA64>(RvaToRaw(v, look));
        for (; ilt && ilt->u1.AddressOfData; ++ilt) {
            if (ilt->u1.Ordinal & IMAGE_ORDINAL_FLAG64) {
                unsigned ord = (unsigned)IMAGE_ORDINAL64(ilt->u1.Ordinal);
                const char* name = "?";
                for (auto& e : exports) if (e.ordinal == ord) { name = e.name.c_str(); break; }
                bool covered = (ord >= ordBase && ord <= maxOrd) && name[0] != '?';
                printf("    ordinal #%-4u (%-45s) %s\n", ord, name, covered ? "OK" : "MISSING!");
                if (!covered) ok = false;
            } else {
                auto* hn = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(RvaToRaw(v, (DWORD)ilt->u1.AddressOfData));
                if (!hn) { ok = false; continue; }
                bool covered = false;
                for (auto& e : exports) if (e.name == (const char*)hn->Name) { covered = true; break; }
                printf("    name    %-49s %s\n", hn->Name, covered ? "OK" : "MISSING!");
                if (!covered) ok = false;
            }
        }
    }
    return ok;
}

static bool WriteText(const char* path, const std::string& content)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) return false;
    fwrite(content.data(), 1, content.size(), f);
    fclose(f);
    return true;
}

int wmain(int argc, wchar_t* argv[])
{
    // 1) 定位本机真身（绝不写死盘符）
    wchar_t sysDir[MAX_PATH];
    UINT n = GetSystemDirectoryW(sysDir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { printf("[gen_exports] GetSystemDirectoryW failed\n"); return 1; }
    std::wstring realPath = std::wstring(sysDir) + L"\\d3d12.dll";
    printf("[gen_exports] source: %ws\n", realPath.c_str());

    // 2) 解析导出表
    std::vector<BYTE> img;
    if (!ReadFileAll(realPath.c_str(), img)) { printf("[gen_exports] cannot read d3d12.dll\n"); return 1; }
    PeView v{};
    if (!MakePeView(img, v)) { printf("[gen_exports] invalid PE\n"); return 1; }
    std::vector<ExportEntry> exports;
    unsigned ordBase = 0;
    if (!ParseExports(v, exports, ordBase)) { printf("[gen_exports] no exports parsed\n"); return 1; }
    printf("[gen_exports] %zu exports, ordinal base = %u\n", exports.size(), ordBase);

    // 3) 生成三个文件（CRLF，ml64/cl 都兼容）
    std::string pragmas = "// AUTO-GENERATED by gen_exports.exe - do not edit\r\n";
    std::string thunks  = "; AUTO-GENERATED by gen_exports.exe - do not edit\r\n";
    std::string names   = "// AUTO-GENERATED by gen_exports.exe - do not edit\r\n";
    {
        char line[512];
        names += "#define AGILITY_EXPORT_COUNT " + std::to_string(exports.size()) + "\r\n";
        names += "static const char* const kExportNames[AGILITY_EXPORT_COUNT] = {\r\n";
        for (size_t i = 0; i < exports.size(); ++i) {
            sprintf_s(line, "#pragma comment(linker, \"/export:%s,@%u\")\r\n",
                      exports[i].name.c_str(), exports[i].ordinal);
            pragmas += line;
            sprintf_s(line, "THUNK %s, %zu\r\n", exports[i].name.c_str(), i);
            thunks += line;
            sprintf_s(line, "    \"%s\",\r\n", exports[i].name.c_str());
            names += line;
        }
        names += "};\r\n";
    }
    if (!WriteText("generated_pragmas.h", pragmas) ||
        !WriteText("generated_thunks.inc", thunks) ||
        !WriteText("generated_names.h", names)) {
        printf("[gen_exports] failed to write generated files\n");
        return 1;
    }
    printf("[gen_exports] generated: generated_pragmas.h / generated_thunks.inc / generated_names.h\n");

    // 4) 可选：校验 Unity.dll 的导入覆盖
    if (argc >= 2) {
        std::wstring unityDll = std::wstring(argv[1]) + L"\\Unity.dll";
        if (!CheckImportsAgainst(unityDll.c_str(), exports, ordBase)) {
            printf("[gen_exports] FAIL: Unity.dll imports not fully covered\n");
            return 2;
        }
    }

    printf("[gen_exports] OK\n");
    return 0;
}
