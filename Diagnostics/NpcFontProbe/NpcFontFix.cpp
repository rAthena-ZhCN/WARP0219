
#include <windows.h>
#include <cstdint>
#include <cstring>

static HMODULE g_main;
static void* g_realTextOutW = nullptr;
static DWORD g_highCaller = 0;
static void* g_renderTramp = nullptr;
static void* g_renderTarget = nullptr;

struct FontEntry
{
    int height;
    int weight;
    int charset;
    char face[LF_FACESIZE];
    HFONT font;
};
static FontEntry g_fonts[8];
static int g_fontCount = 0;

// The client's high-level text renderer starts at RVA 0x150190.
// Capture its caller before the prologue so we can identify the NPC window.
__declspec(naked) void MyRenderEntry()
{
    __asm
    {
        mov eax, [esp]
        mov dword ptr [g_highCaller], eax
        jmp dword ptr [g_renderTramp]
    }
}

static void InstallRenderHook()
{
    g_renderTarget = (BYTE*)GetModuleHandleA(NULL) + 0x150190;
    DWORD oldProt = 0;
    VirtualProtect(g_renderTarget, 6, PAGE_EXECUTE_READWRITE, &oldProt);
    g_renderTramp = VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_renderTramp) return;
    memcpy(g_renderTramp, g_renderTarget, 6);
    BYTE* tramp = (BYTE*)g_renderTramp;
    tramp[6] = 0xE9;
    *(int32_t*)(tramp + 7) = (int32_t)((BYTE*)g_renderTarget + 6 - (tramp + 11));
    BYTE* target = (BYTE*)g_renderTarget;
    target[0] = 0xE9;
    *(int32_t*)(target + 1) = (int32_t)((BYTE*)MyRenderEntry - (target + 5));
    VirtualProtect(g_renderTarget, 6, oldProt, &oldProt);
}

static HFONT GetNpcFont(HDC hdc)
{
    HFONT cur = (HFONT)GetCurrentObject(hdc, OBJ_FONT);
    LOGFONTA lf = {};
    if (!cur || !GetObjectA(cur, sizeof(lf), &lf))
        return nullptr;

    int targetHeight = lf.lfHeight;
    if (targetHeight < 0) targetHeight -= 1; // -14 -> -15
    else targetHeight += 1;                 // 14 -> 15

    for (int i = 0; i < g_fontCount; ++i)
    {
        FontEntry& e = g_fonts[i];
        if (e.height == targetHeight && e.weight == lf.lfWeight &&
            e.charset == lf.lfCharSet && strcmp(e.face, lf.lfFaceName) == 0)
            return e.font;
    }
    if (g_fontCount >= 8)
        return nullptr;

    LOGFONTA big = lf;
    big.lfHeight = targetHeight;
    HFONT font = CreateFontIndirectA(&big);
    if (!font) return nullptr;

    FontEntry& e = g_fonts[g_fontCount++];
    e.height = targetHeight;
    e.weight = lf.lfWeight;
    e.charset = lf.lfCharSet;
    strncpy_s(e.face, sizeof(e.face), lf.lfFaceName, _TRUNCATE);
    e.font = font;
    return font;
}

static BOOL WINAPI MyTextOutW(HDC hdc, int x, int y, LPCWSTR s, int c)
{
    typedef BOOL (WINAPI* Fn)(HDC,int,int,LPCWSTR,int);
    Fn real = (Fn)g_realTextOutW;

    // 0x00A843DF: NPC dialogue body
    // 0x00A8501C: NPC dialogue options
    bool npc = (g_highCaller == 0x00A843DF || g_highCaller == 0x00A8501C);
    if (!npc)
        return real(hdc, x, y, s, c);

    HFONT big = GetNpcFont(hdc);
    if (!big)
        return real(hdc, x, y, s, c);

    HGDIOBJ old = SelectObject(hdc, big);
    BOOL result = real(hdc, x, y, s, c);
    if (old) SelectObject(hdc, old);
    return result;
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
    g_main = GetModuleHandleA(NULL);
    InstallRenderHook();
    g_realTextOutW = HookIat(g_main, "gdi32.dll", "TextOutW", (void*)MyTextOutW);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hinst);
        HANDLE t = CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}

extern "C" __declspec(dllexport) void NpcFontProbeInit() {}
