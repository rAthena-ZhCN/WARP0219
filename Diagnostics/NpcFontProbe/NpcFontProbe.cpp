
#include <windows.h>
#include <intrin.h>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <cstdio>

static CRITICAL_SECTION g_cs;
static char g_logPath[MAX_PATH];
static HMODULE g_main;
static void* g_realCreateFontA = nullptr;
static void* g_realCreateFontIndirectA = nullptr;
static void* g_realTextOutA = nullptr;
static void* g_realTextOutW = nullptr;
static void* g_realExtTextOutA = nullptr;
static void* g_realDrawTextA = nullptr;

static DWORD g_seen[2048];
static int g_seenCount = 0;
static DWORD g_sampleCount[2048];

static void InitLogPath()
{
    char exe[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    char* slash = strrchr(exe, '\\');
    if (slash) *(slash + 1) = 0;
    wsprintfA(g_logPath, "%sNpcFontProbe.log", exe);
}

static void Log(const char* fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = wvsprintfA(buf, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    EnterCriticalSection(&g_cs);
    HANDLE h = CreateFileA(g_logPath, FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        WriteFile(h, buf, (DWORD)n, &written, NULL);
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_cs);
}

static void HexSample(const unsigned char* p, int len, char* out, int outSize)
{
    int used = 0;
    if (!p) { out[0] = 0; return; }
    if (len > 80) len = 80;
    for (int i = 0; i < len && used + 4 < outSize; ++i)
        used += wsprintfA(out + used, "%02X ", p[i]);
    out[used] = 0;
}

static void LogFontInfo(HDC hdc, const char* tag, DWORD caller)
{
    HFONT hf = (HFONT)GetCurrentObject(hdc, OBJ_FONT);
    LOGFONTA lf = {};
    if (hf && GetObjectA(hf, sizeof(lf), &lf))
        Log("%s caller=%08X font=%08X h=%d w=%d weight=%d charset=%u face=%s\n",
            tag, caller, (DWORD)(uintptr_t)hf, lf.lfHeight, lf.lfWidth, lf.lfWeight,
            lf.lfCharSet, lf.lfFaceName);
    else
        Log("%s caller=%08X font=%08X (no LOGFONT)\n", tag, caller, (DWORD)(uintptr_t)hf);
}

static bool LogCallerOnce(DWORD caller)
{
    for (int i = 0; i < g_seenCount; ++i)
    {
        if (g_seen[i] == caller)
        {
            if (g_sampleCount[i] < 50) { g_sampleCount[i]++; return true; }
            return false;
        }
    }
    if (g_seenCount < 2048)
    {
        g_seen[g_seenCount] = caller;
        g_sampleCount[g_seenCount] = 1;
        g_seenCount++;
        return true;
    }
    return false;
}

static HFONT WINAPI MyCreateFontA(
    int cHeight, int cWidth, int cEscapement, int cOrientation, int cWeight,
    DWORD bItalic, DWORD bUnderline, DWORD bStrikeOut, DWORD iCharSet,
    DWORD iOutPrecision, DWORD iClipPrecision, DWORD iQuality,
    DWORD iPitchAndFamily, LPCSTR pszFaceName)
{
    DWORD caller = (DWORD)(uintptr_t)_ReturnAddress();
    Log("FONTA caller=%08X h=%d weight=%d charset=%u face=%s\n",
        caller, cHeight, cWeight, iCharSet, pszFaceName ? pszFaceName : "(null)");
    typedef HFONT (WINAPI* Fn)(int,int,int,int,int,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,LPCSTR);
    return ((Fn)g_realCreateFontA)(cHeight, cWidth, cEscapement, cOrientation, cWeight,
        bItalic, bUnderline, bStrikeOut, iCharSet, iOutPrecision, iClipPrecision,
        iQuality, iPitchAndFamily, pszFaceName);
}

static HFONT WINAPI MyCreateFontIndirectA(const LOGFONTA* lf)
{
    DWORD caller = (DWORD)(uintptr_t)_ReturnAddress();
    if (lf)
        Log("FONTI caller=%08X h=%d weight=%d charset=%u face=%s\n",
            caller, lf->lfHeight, lf->lfWeight, lf->lfCharSet, lf->lfFaceName);
    else
        Log("FONTI caller=%08X (null)\n", caller);
    typedef HFONT (WINAPI* Fn)(const LOGFONTA*);
    return ((Fn)g_realCreateFontIndirectA)(lf);
}

static BOOL WINAPI MyTextOutA(HDC hdc, int x, int y, LPCSTR s, int c)
{
    DWORD caller = (DWORD)(uintptr_t)_ReturnAddress();
    if (s && LogCallerOnce(caller))
    {
        char hex[512] = {0};
        HexSample((const unsigned char*)s, c < 0 ? 80 : c, hex, sizeof(hex));
        LogFontInfo(hdc, "TEXTOUT", caller);
        Log("TEXTOUT caller=%08X x=%d y=%d len=%d text=%s\n", caller, x, y, c, hex);
    }
    typedef BOOL (WINAPI* Fn)(HDC,int,int,LPCSTR,int);
    return ((Fn)g_realTextOutA)(hdc, x, y, s, c);
}





static DWORD g_wrapperCaller = 0;
static void* g_wrapperTramp = nullptr;
static void* g_wrapperTarget = nullptr;

// The client's generic text wrapper starts at RVA 0x150680. At entry,
// [esp] is the high-level drawing caller. Capture it before the wrapper prologue.
__declspec(naked) void MyWrapperEntry()
{
    __asm
    {
        mov eax, [esp]
        mov dword ptr [g_wrapperCaller], eax
        jmp dword ptr [g_wrapperTramp]
    }
}

static void InstallWrapperHook()
{
    g_wrapperTarget = (BYTE*)GetModuleHandleA(NULL) + 0x150680;
    DWORD oldProt = 0;
    VirtualProtect(g_wrapperTarget, 6, PAGE_EXECUTE_READWRITE, &oldProt);

    g_wrapperTramp = VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_wrapperTramp)
        return;

    memcpy(g_wrapperTramp, g_wrapperTarget, 6);
    BYTE* tramp = (BYTE*)g_wrapperTramp;
    tramp[6] = 0xE9;
    *(int32_t*)(tramp + 7) = (int32_t)((BYTE*)g_wrapperTarget + 6 - (tramp + 11));

    BYTE* target = (BYTE*)g_wrapperTarget;
    target[0] = 0xE9;
    *(int32_t*)(target + 1) = (int32_t)((BYTE*)MyWrapperEntry - (target + 5));
    VirtualProtect(g_wrapperTarget, 6, oldProt, &oldProt);
}

static BOOL WINAPI MyTextOutW(HDC hdc, int x, int y, LPCWSTR s, int c)
{
    DWORD caller = (DWORD)(uintptr_t)_ReturnAddress();
    DWORD realCaller = g_wrapperCaller ? g_wrapperCaller : caller;

    if (s && LogCallerOnce(realCaller))
    {
        char gbk[1024] = {0};
        int chars = c;
        if (chars < 0) chars = -1;
        else if (chars > 1024) chars = 1024;
        int n = WideCharToMultiByte(936, 0, s, chars, gbk, sizeof(gbk) - 1, NULL, NULL);
        if (n < 0) n = 0;
        gbk[n] = 0;
        char hex[512] = {0};
        HexSample((const unsigned char*)gbk, n, hex, sizeof(hex));
        LogFontInfo(hdc, "TEXTOUTW", realCaller);
        Log("TEXTOUTW caller=%08X raw=%08X x=%d y=%d len=%d gbk=%s text=%s\n",
            realCaller, caller, x, y, c, gbk, hex);
    }
    typedef BOOL (WINAPI* Fn)(HDC,int,int,LPCWSTR,int);
    return ((Fn)g_realTextOutW)(hdc, x, y, s, c);
}

static BOOL WINAPI MyExtTextOutA(HDC hdc, int x, int y, UINT opts, const RECT* rc, LPCSTR s, UINT c, const INT* dx)
{
    DWORD caller = (DWORD)(uintptr_t)_ReturnAddress();
    if (s && LogCallerOnce(caller))
    {
        char hex[512] = {0};
        HexSample((const unsigned char*)s, (int)c, hex, sizeof(hex));
        LogFontInfo(hdc, "EXTTEXTOUT", caller);
        Log("EXTTEXTOUT caller=%08X x=%d y=%d len=%u text=%s\n", caller, x, y, c, hex);
    }
    typedef BOOL (WINAPI* Fn)(HDC,int,int,UINT,const RECT*,LPCSTR,UINT,const INT*);
    return ((Fn)g_realExtTextOutA)(hdc, x, y, opts, rc, s, c, dx);
}

static int WINAPI MyDrawTextA(HDC hdc, LPCSTR s, int c, LPRECT rc, UINT fmt)
{
    DWORD caller = (DWORD)(uintptr_t)_ReturnAddress();
    if (s && LogCallerOnce(caller))
    {
        char hex[512] = {0};
        HexSample((const unsigned char*)s, c < 0 ? 80 : c, hex, sizeof(hex));
        LogFontInfo(hdc, "DRAWTEXT", caller);
        Log("DRAWTEXT caller=%08X len=%d fmt=%08X text=%s\n", caller, c, fmt, hex);
    }
    typedef int (WINAPI* Fn)(HDC,LPCSTR,int,LPRECT,UINT);
    return ((Fn)g_realDrawTextA)(hdc, s, c, rc, fmt);
}

static void* HookIat(HMODULE mod, const char* targetDll, const char* funcName, void* hook)
{
    if (!mod) return nullptr;
    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    IMAGE_IMPORT_DESCRIPTOR* desc = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress);
    for (; desc->Name; ++desc)
    {
        const char* dll = (const char*)(base + desc->Name);
        if (_stricmp(dll, targetDll) != 0) continue;
        IMAGE_THUNK_DATA* thunk = (IMAGE_THUNK_DATA*)(base + desc->FirstThunk);
        IMAGE_THUNK_DATA* orig = desc->OriginalFirstThunk
            ? (IMAGE_THUNK_DATA*)(base + desc->OriginalFirstThunk) : thunk;
        for (; thunk->u1.Function; ++thunk, ++orig)
        {
            if (!(orig->u1.Ordinal & IMAGE_ORDINAL_FLAG))
            {
                IMAGE_IMPORT_BY_NAME* name = (IMAGE_IMPORT_BY_NAME*)(base + orig->u1.AddressOfData);
                if (strcmp((const char*)name->Name, funcName) == 0)
                {
                    void* old = (void*)thunk->u1.Function;
                    DWORD oldProt = 0;
                    VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProt);
                    thunk->u1.Function = (ULONG_PTR)hook;
                    VirtualProtect(&thunk->u1.Function, sizeof(void*), oldProt, &oldProt);
                    return old;
                }
            }
        }
    }
    return nullptr;
}

static DWORD WINAPI InitThread(LPVOID)
{
    InitLogPath();
    g_main = GetModuleHandleA(NULL);
    Log("=== NpcFontProbe start pid=%u base=%08X log=%s ===\n",
        GetCurrentProcessId(), (DWORD)(uintptr_t)g_main, g_logPath);
    g_realCreateFontA = HookIat(g_main, "gdi32.dll", "CreateFontA", (void*)MyCreateFontA);
    g_realCreateFontIndirectA = HookIat(g_main, "gdi32.dll", "CreateFontIndirectA", (void*)MyCreateFontIndirectA);
    g_realTextOutA = HookIat(g_main, "gdi32.dll", "TextOutA", (void*)MyTextOutA);
    g_realTextOutW = HookIat(g_main, "gdi32.dll", "TextOutW", (void*)MyTextOutW);
    g_realExtTextOutA = HookIat(g_main, "gdi32.dll", "ExtTextOutA", (void*)MyExtTextOutA);
    g_realDrawTextA = HookIat(g_main, "user32.dll", "DrawTextA", (void*)MyDrawTextA);
    InstallWrapperHook();
    Log("hooks: FONTA=%p FONTI=%p TEXTOUT=%p TEXTOUTW=%p EXTTEXTOUT=%p DRAWTEXT=%p\n",
        g_realCreateFontA, g_realCreateFontIndirectA, g_realTextOutA, g_realTextOutW, g_realExtTextOutA, g_realDrawTextA);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hinst);
        InitializeCriticalSection(&g_cs);
        HANDLE t = CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}

extern "C" __declspec(dllexport) void NpcFontProbeInit() {}
