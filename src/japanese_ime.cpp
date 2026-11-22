/**
 * japanese_ime - lets Ashita / VanaCore addons take Japanese IME input in their own ImGui text
 *                boxes while the game's chat line stays closed.
 * Copyright (C) 2026 Tsukasa
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <Windows.h>
#include <commctrl.h>
#include <imm.h>
#include <new>
#include <string.h>
#include <stdio.h>

#include "Ashita.h"

// ---------------------------------------------------------------------------------------------
// Notes on the binary layout
//
// The host is built with MSVC. This plugin is built with mingw-w64 (GCC, i686). Both use
// __thiscall for member functions on 32-bit Windows and the same single-inheritance vtable
// layout, as long as there are no overloaded virtuals (there are none in IPluginBase).
//
// The one difference is the virtual destructor: MSVC puts one "scalar deleting destructor"
// slot after the last virtual (it takes a flags argument and frees when bit 0 is set), while
// GCC puts two slots there. So this class derives from IPluginBase directly and puts its own
// virtual in that slot with MSVC's signature. Memory comes from the process heap so it can be
// freed here without depending on any C runtime.
// ---------------------------------------------------------------------------------------------

namespace
{
    constexpr const char* PLUGIN_NAME    = "japanese_ime";
    constexpr const char* EVENT_RESULT   = "japanese_ime_text";    // to the owner: "<id>\t<committed text, UTF-8>"
    constexpr const char* EVENT_COMP     = "japanese_ime_comp";    // to the owner: "<id>\t<text being converted>" (empty = cleared)
    constexpr const char* EVENT_CTL      = "japanese_ime_ctl";     // from an addon: "on <id>" / "off <id>" (a text box of its own is active)
    constexpr const char* EVENT_STATE    = "japanese_ime_state";   // to the addons: "on <owner id>" / "off"
    constexpr UINT_PTR    SUBCLASS_ID    = 0x4A494D45;             // 'JIME'
    constexpr size_t      OWNER_MAX      = 8;                      // addons asking at once (the last one wins)
    constexpr size_t      ID_MAX         = 32;
    constexpr DWORD       KEY_GRACE_MS   = 500;                    // keys stay hidden this long after the box is left (the Enter that left it)
    constexpr int32_t     CHAT_MODE      = 1;
    constexpr size_t      QUEUE_MAX      = 64;
    constexpr size_t      TEXT_MAX       = 512;                    // UTF-16 units per item

    enum class ItemKind : uint32_t { Result, Comp, Char, Note };

    struct Item
    {
        ItemKind kind;
        uint32_t len;
        wchar_t  text[TEXT_MAX];
    };

    struct Stats
    {
        volatile LONG start_comp;
        volatile LONG composition;
        volatile LONG result;
        volatile LONG end_comp;
        volatile LONG ime_char;
        volatile LONG wm_char;
        volatile LONG ime_notify;
        volatile LONG ime_setcontext;
        volatile LONG swallowed;
        volatile LONG routed;
    };

    UINT g_remove_msg = 0;
}

class JapaneseIME;
static JapaneseIME* volatile g_instance = nullptr;  // the live plugin object, or null after Release

// ---------------------------------------------------------------------------------------------
// IME calls the game makes (0.3). FFXiMain.dll's import slots for a few IMM32 functions point
// here while the plugin is loaded: every call is noted for the log, and while the addon's box is
// active (g_block) the calls that would close the IME, change its mode or throw the text away are
// skipped. The user's own key (半角/全角) is handled by the IME itself and doesn't come this way.
// ---------------------------------------------------------------------------------------------
namespace
{
    typedef BOOL (WINAPI* PFN_SetOpen)(HIMC, BOOL);
    typedef BOOL (WINAPI* PFN_SetConv)(HIMC, DWORD, DWORD);
    typedef BOOL (WINAPI* PFN_Notify)(HIMC, DWORD, DWORD, DWORD);
    typedef BOOL (WINAPI* PFN_SetCompA)(HIMC, DWORD, LPVOID, DWORD, LPVOID, DWORD);
    typedef BOOL (WINAPI* PFN_GetOpen)(HIMC);
    typedef LONG (WINAPI* PFN_GetCompA)(HIMC, DWORD, LPVOID, DWORD);
    typedef DWORD (WINAPI* PFN_GetCandA)(HIMC, DWORD, LPCANDIDATELIST, DWORD);
    typedef DWORD (WINAPI* PFN_GetCandCountA)(HIMC, LPDWORD);
    typedef UINT (WINAPI* PFN_GetVKey)(HWND);
    typedef BOOL (WINAPI* PFN_GetConv)(HIMC, LPDWORD, LPDWORD);

    volatile bool  g_block = false;
    volatile LONG  g_blocked = 0;
    volatile LONG  g_calls = 0;

    struct IatHook
    {
        const char* name;
        void*       hook;
        void*       real;
        void**      slot;
    };

    void NoteImm(const char* fn, DWORD a, DWORD b, DWORD c, void* ret, bool blocked);
    void NoteText(const char* text);
    volatile LONG g_stackDumps = 0;  // reset when the addon turns on: one stack per box session
}

// 0.8 diagnosis: which code path made the game open its chat line. Scans the raw stack for return
// addresses (dwords pointing just after a call instruction in a loaded module) and logs them.
static bool LooksLikeReturn(uintptr_t a, char* out, size_t outLen)
{
    HMODULE mod = nullptr;
    if (a < 0x10000 || !::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &mod) || !mod)
        return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!::VirtualQuery((LPCVOID)(a - 8), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0)
        return false;
    const unsigned char* p = (const unsigned char*)a;
    const bool call = p[-5] == 0xE8 || (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) || (p[-2] == 0xFF && (p[-1] & 0xF8) == 0xD0) ||
                      (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) || (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10);
    if (!call)
        return false;
    char path[MAX_PATH] = {};
    ::GetModuleFileNameA(mod, path, MAX_PATH);
    const char* name = strrchr(path, '\\');
    name = name ? name + 1 : path;
    snprintf(out, outLen, "%s+0x%lx", name, (unsigned long)(a - (uintptr_t)mod));
    return true;
}

static volatile UINT   g_defMsg = 0;   // message the game is passing to DefWindowProcA right now (0.9)
static volatile WPARAM g_defWp  = 0;
static void* g_stackRets[6] = {};
static void DumpStack(const char* why, void* ret)
{
    if (g_stackDumps == 0)
        memset(g_stackRets, 0, sizeof(g_stackRets));
    for (auto r : g_stackRets)
        if (r == ret)
            return;
    LONG n = ::InterlockedIncrement(&g_stackDumps);
    if (n > 6)
        return;
    g_stackRets[n - 1] = ret;
    uintptr_t* sp = (uintptr_t*)__builtin_frame_address(0);
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    uintptr_t* top = (uintptr_t*)tib->StackBase;
    char line[180];
    if (g_defMsg)
        snprintf(line, sizeof(line), "stack (%s; inside the game's DefWindowProcA(0x%x, 0x%lx)):", why, g_defMsg, (unsigned long)g_defWp);
    else
        snprintf(line, sizeof(line), "stack (%s):", why);
    size_t len = strlen(line);
    int found = 0, lines = 0;
    for (int i = 0; i < 2048 && sp + i < top && found < 32 && lines < 6; i++)
    {
        char one[64];
        if (!LooksLikeReturn(sp[i], one, sizeof(one)))
            continue;
        found++;
        if (len + strlen(one) + 2 >= sizeof(line))
        {
            NoteText(line);
            lines++;
            snprintf(line, sizeof(line), "stack ..:");
            len = strlen(line);
        }
        snprintf(line + len, sizeof(line) - len, " %s", one);
        len = strlen(line);
    }
    if (lines < 6)
        NoteText(line);
}

static BOOL WINAPI HookSetOpen(HIMC h, BOOL open);
static BOOL WINAPI HookSetConv(HIMC h, DWORD conv, DWORD sent);
static BOOL WINAPI HookNotify(HIMC h, DWORD action, DWORD index, DWORD value);
static BOOL WINAPI HookSetCompA(HIMC h, DWORD index, LPVOID comp, DWORD compLen, LPVOID read, DWORD readLen);
static BOOL WINAPI HookGetOpen(HIMC h);
static LONG WINAPI HookGetCompA(HIMC h, DWORD index, LPVOID buf, DWORD len);
static DWORD WINAPI HookGetCandA(HIMC h, DWORD index, LPCANDIDATELIST list, DWORD len);
static DWORD WINAPI HookGetCandCountA(HIMC h, LPDWORD count);
static UINT WINAPI HookGetVKey(HWND hwnd);
static BOOL WINAPI HookGetConv(HIMC h, LPDWORD conv, LPDWORD sent);

static IatHook g_hooks[] = {
    {"ImmSetOpenStatus", (void*)&HookSetOpen, nullptr, nullptr},
    {"ImmSetConversionStatus", (void*)&HookSetConv, nullptr, nullptr},
    {"ImmNotifyIME", (void*)&HookNotify, nullptr, nullptr},
    {"ImmSetCompositionStringA", (void*)&HookSetCompA, nullptr, nullptr},
    // 0.6: what the game reads. While the addon's box is active the game is told the IME is shut and
    // has nothing in it, so it doesn't take the composition or its result into its own chat line.
    {"ImmGetOpenStatus", (void*)&HookGetOpen, nullptr, nullptr},
    {"ImmGetCompositionStringA", (void*)&HookGetCompA, nullptr, nullptr},
    {"ImmGetCandidateListA", (void*)&HookGetCandA, nullptr, nullptr},
    {"ImmGetCandidateListCountA", (void*)&HookGetCandCountA, nullptr, nullptr},
    {"ImmGetVirtualKey", (void*)&HookGetVKey, nullptr, nullptr},
    {"ImmGetConversionStatus", (void*)&HookGetConv, nullptr, nullptr},
};

static BOOL WINAPI HookGetOpen(HIMC h)
{
    const bool block = g_block;
    NoteImm("ImmGetOpenStatus", 0, 0, 0, __builtin_return_address(0), block);
    if (block)
    {
        DumpStack("ImmGetOpenStatus from the game while the box is active", __builtin_return_address(0));
        return FALSE;
    }
    auto real = (PFN_GetOpen)g_hooks[4].real;
    return real ? real(h) : FALSE;
}

static LONG WINAPI HookGetCompA(HIMC h, DWORD index, LPVOID buf, DWORD len)
{
    const bool block = g_block;
    NoteImm("ImmGetCompositionStringA", index, len, 0, __builtin_return_address(0), block);
    if (block)
        return 0;
    auto real = (PFN_GetCompA)g_hooks[5].real;
    return real ? real(h, index, buf, len) : IMM_ERROR_GENERAL;
}

static DWORD WINAPI HookGetCandA(HIMC h, DWORD index, LPCANDIDATELIST list, DWORD len)
{
    const bool block = g_block;
    NoteImm("ImmGetCandidateListA", index, len, 0, __builtin_return_address(0), block);
    if (block)
        return 0;
    auto real = (PFN_GetCandA)g_hooks[6].real;
    return real ? real(h, index, list, len) : 0;
}

static DWORD WINAPI HookGetCandCountA(HIMC h, LPDWORD count)
{
    const bool block = g_block;
    NoteImm("ImmGetCandidateListCountA", 0, 0, 0, __builtin_return_address(0), block);
    if (block)
    {
        if (count)
            *count = 0;
        return 0;
    }
    auto real = (PFN_GetCandCountA)g_hooks[7].real;
    return real ? real(h, count) : 0;
}

static UINT WINAPI HookGetVKey(HWND hwnd)
{
    const bool block = g_block;
    NoteImm("ImmGetVirtualKey", 0, 0, 0, __builtin_return_address(0), block);
    if (block)
        return VK_PROCESSKEY;
    auto real = (PFN_GetVKey)g_hooks[8].real;
    return real ? real(hwnd) : VK_PROCESSKEY;
}

static BOOL WINAPI HookGetConv(HIMC h, LPDWORD conv, LPDWORD sent)
{
    // Read-only and harmless: passed through, only noted (to see what the game looks at).
    NoteImm("ImmGetConversionStatus", 0, 0, 0, __builtin_return_address(0), false);
    auto real = (PFN_GetConv)g_hooks[9].real;
    return real ? real(h, conv, sent) : FALSE;
}

static BOOL WINAPI HookSetOpen(HIMC h, BOOL open)
{
    const bool block = g_block && !open;
    NoteImm("ImmSetOpenStatus", (DWORD)open, 0, 0, __builtin_return_address(0), block);
    if (block)
    {
        ::InterlockedIncrement(&g_blocked);
        return TRUE;
    }
    auto real = (PFN_SetOpen)g_hooks[0].real;
    return real ? real(h, open) : FALSE;
}

static BOOL WINAPI HookSetConv(HIMC h, DWORD conv, DWORD sent)
{
    const bool block = g_block;
    NoteImm("ImmSetConversionStatus", conv, sent, 0, __builtin_return_address(0), block);
    if (block)
    {
        ::InterlockedIncrement(&g_blocked);
        return TRUE;
    }
    auto real = (PFN_SetConv)g_hooks[1].real;
    return real ? real(h, conv, sent) : FALSE;
}

static BOOL WINAPI HookNotify(HIMC h, DWORD action, DWORD index, DWORD value)
{
    // NI_COMPOSITIONSTR + CPS_CANCEL throws the text being converted away; NI_CLOSECANDIDATE shuts the list.
    const bool block = g_block && (action == NI_COMPOSITIONSTR || action == NI_CLOSECANDIDATE);
    NoteImm("ImmNotifyIME", action, index, value, __builtin_return_address(0), block);
    if (block)
    {
        ::InterlockedIncrement(&g_blocked);
        return TRUE;
    }
    auto real = (PFN_Notify)g_hooks[2].real;
    return real ? real(h, action, index, value) : FALSE;
}

static BOOL WINAPI HookSetCompA(HIMC h, DWORD index, LPVOID comp, DWORD compLen, LPVOID read, DWORD readLen)
{
    const bool block = g_block;
    NoteImm("ImmSetCompositionStringA", index, compLen, readLen, __builtin_return_address(0), block);
    if (block)
    {
        ::InterlockedIncrement(&g_blocked);
        return TRUE;
    }
    auto real = (PFN_SetCompA)g_hooks[3].real;
    return real ? real(h, index, comp, compLen, read, readLen) : FALSE;
}

// The import slot of `name` (from `dll`) in module `mod`: by name first, then by the address it holds.
static void** FindIatSlot(HMODULE mod, const char* dll, const char* name)
{
    if (mod == nullptr)
        return nullptr;
    auto base = reinterpret_cast<BYTE*>(mod);
    auto dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return nullptr;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return nullptr;
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0)
        return nullptr;
    HMODULE immMod = ::GetModuleHandleA(dll);
    void* real     = immMod ? (void*)::GetProcAddress(immMod, name) : nullptr;
    for (auto d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; d++)
    {
        if (_stricmp(reinterpret_cast<char*>(base + d->Name), dll) != 0 || d->FirstThunk == 0)
            continue;
        auto f = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk);
        if (d->OriginalFirstThunk)
        {
            auto o = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->OriginalFirstThunk);
            for (auto ff = f; o->u1.AddressOfData; o++, ff++)
            {
                if (IMAGE_SNAP_BY_ORDINAL(o->u1.Ordinal))
                    continue;
                auto ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + o->u1.AddressOfData);
                if (strcmp(reinterpret_cast<char*>(ibn->Name), name) == 0)
                    return reinterpret_cast<void**>(&ff->u1.Function);
            }
        }
        if (real)
        {
            for (auto ff = f; ff->u1.Function; ff++)
            {
                if ((void*)ff->u1.Function == real)
                    return reinterpret_cast<void**>(&ff->u1.Function);
            }
        }
    }
    return nullptr;
}

static bool WriteSlot(void** slot, void* value)
{
    DWORD old = 0;
    if (!::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
        return false;
    ::InterlockedExchangePointer(slot, value);
    ::VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

// Returns how many of the hooks are in place.
// ---------------------------------------------------------------------------------------------
// The game's message loop (0.7). FFXiMain.dll takes its window messages with PeekMessageA and
// looks at keys there, before they are dispatched: with the IME on, the Space / Enter the IME uses
// (WM_KEYDOWN VK_PROCESSKEY) and the IME's own messages opened its chat line from there. While the
// addon's box is active those messages are translated and dispatched here instead (so the IME and
// the window still get them) and the game's loop is handed the next message.
// ---------------------------------------------------------------------------------------------
typedef BOOL (WINAPI* PFN_PeekA)(LPMSG, HWND, UINT, UINT, UINT);
static BOOL WINAPI HookPeekA(LPMSG msg, HWND hwnd, UINT mn, UINT mx, UINT flags);
static IatHook g_peek = {"PeekMessageA", (void*)&HookPeekA, nullptr, nullptr};
static volatile LONG g_peekDepth = 0;
static volatile LONG g_loopHidden = 0;

static bool IsImeLoopMessage(const MSG* m)
{
    switch (m->message)
    {
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
            return m->wParam == VK_PROCESSKEY;
        case WM_IME_STARTCOMPOSITION:
        case WM_IME_ENDCOMPOSITION:
        case WM_IME_COMPOSITION:
            return true;
        default:
            return m->message >= WM_IME_SETCONTEXT && m->message <= WM_IME_KEYUP;
    }
}

static BOOL WINAPI HookPeekA(LPMSG msg, HWND hwnd, UINT mn, UINT mx, UINT flags)
{
    auto real = (PFN_PeekA)g_peek.real;
    if (real == nullptr)
        return FALSE;
    ::InterlockedIncrement(&g_peekDepth);
    BOOL got = FALSE;
    for (int guard = 0; guard < 64; guard++)
    {
        got = real(msg, hwnd, mn, mx, flags);
        if (got && g_block && msg && (flags & PM_REMOVE) &&
            (msg->message == WM_KEYDOWN || msg->message == WM_KEYUP || msg->message == WM_CHAR || msg->message == WM_SYSKEYDOWN))
            NoteImm("PeekMessageA", msg->message, (DWORD)msg->wParam, 0, __builtin_return_address(0), IsImeLoopMessage(msg));
        if (!got || !g_block || msg == nullptr || !IsImeLoopMessage(msg))
            break;
        if ((flags & PM_REMOVE) == 0)
        {
            // Only looked at: take the same message off the queue (same filters) first.
            MSG taken = {};
            if (!real(&taken, hwnd, mn, mx, flags | PM_REMOVE))
            {
                got = FALSE;
                break;
            }
            *msg = taken;
        }
        ::InterlockedIncrement(&g_loopHidden);
        ::TranslateMessage(msg);
        ::DispatchMessageA(msg);
        got = FALSE;
    }
    ::InterlockedDecrement(&g_peekDepth);
    return got;
}

// ---------------------------------------------------------------------------------------------
// The keyboard state the game reads through user32 (0.8): GetAsyncKeyState / GetKeyboardState.
// DirectInput is hidden by the addon, but with the IME on the game opened its chat line from Space /
// the confirming Enter anyway. While the addon's box is active, the keys a text box uses read as up;
// a key hidden that way stays up for the game until it is really released.
// ---------------------------------------------------------------------------------------------
typedef SHORT (WINAPI* PFN_Async)(int);
typedef BOOL (WINAPI* PFN_KbState)(PBYTE);
static SHORT WINAPI HookAsync(int vk);
static BOOL WINAPI HookKbState(PBYTE keys);
static IatHook g_async   = {"GetAsyncKeyState", (void*)&HookAsync, nullptr, nullptr};
static IatHook g_kbstate = {"GetKeyboardState", (void*)&HookKbState, nullptr, nullptr};
static volatile bool g_keyHeld[256] = {};
static volatile LONG g_keysHidden = 0;

static bool IsTextKey(int vk)
{
    switch (vk)
    {
        case VK_SPACE: case VK_RETURN: case VK_ESCAPE: case VK_BACK: case VK_TAB:
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
        case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        case VK_PROCESSKEY: case VK_CONVERT: case VK_NONCONVERT:
            return true;
        default:
            return false;
    }
}

// true: the game is to see this key as up (now: really down?)
static bool HideKey(int vk, bool down)
{
    vk &= 0xFF;
    if (!down)
    {
        g_keyHeld[vk] = false;
        return false;
    }
    if (g_block && IsTextKey(vk))
        g_keyHeld[vk] = true;
    return g_keyHeld[vk];
}

static SHORT WINAPI HookAsync(int vk)
{
    auto real = (PFN_Async)g_async.real;
    SHORT r = real ? real(vk) : 0;
    if (HideKey(vk, (r & 0x8000) != 0))
    {
        ::InterlockedIncrement(&g_keysHidden);
        NoteImm("GetAsyncKeyState", (DWORD)vk, 0, 0, __builtin_return_address(0), true);
        return 0;
    }
    return r;
}

static BOOL WINAPI HookKbState(PBYTE keys)
{
    auto real = (PFN_KbState)g_kbstate.real;
    BOOL ok = real ? real(keys) : FALSE;
    if (!ok || keys == nullptr)
        return ok;
    for (int vk = 1; vk < 256; vk++)
    {
        if (HideKey(vk, (keys[vk] & 0x80) != 0))
        {
            keys[vk] &= 0x7F;
            ::InterlockedIncrement(&g_keysHidden);
            NoteImm("GetKeyboardState", (DWORD)vk, 0, 0, __builtin_return_address(0), true);
        }
    }
    return ok;
}

// ---------------------------------------------------------------------------------------------
// DirectInput keys (1.0). The game reads its key binds through DirectInput (Space opens the chat
// line, Enter the menu). While an addon's box is active these keys are taken out of what the game
// reads, through the SDK's keyboard callbacks (called after the real GetDeviceData /
// GetDeviceState, like the thirdparty plugin uses them). The box itself still gets them, as
// window messages. A key pressed while the box was active stays hidden until it is released, and
// for a moment after the box is left (the Enter that confirmed and left it).
// ---------------------------------------------------------------------------------------------
volatile bool  g_keysOn = false;        // an addon's box is active (and the game chat is closed)
volatile DWORD g_keysOffAt = 0;         // GetTickCount when g_keysOn last went off
static volatile bool g_dikHeld[256] = {};
static volatile LONG g_dikHidden = 0;
static volatile LONG g_dikCalls = 0;

static bool IsHiddenDik(DWORD k)
{
    switch (k & 0xFF)
    {
        case 0x39: // Space
        case 0x1C: // Enter
        case 0x9C: // numpad Enter
        case 0x01: // Esc
        case 0x0E: // Backspace
        case 0x0F: // Tab
        case 0xC8: case 0xD0: case 0xCB: case 0xCD: // arrows
        case 0xD3: // Delete
        case 0xC7: // Home
        case 0xCF: // End
            return true;
        default:
            return false;
    }
}

static bool KeysHiding(void)
{
    if (g_keysOn)
        return true;
    return (DWORD)(::GetTickCount() - g_keysOffAt) < KEY_GRACE_MS;
}

static HRESULT __stdcall KeyDataCb(IDirectInputDevice8A* device, DWORD cbObjectData, LPDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD dwFlags, DWORD dwOriginalCount)
{
    (void)device;
    (void)dwFlags;
    (void)dwOriginalCount;
    ::InterlockedIncrement(&g_dikCalls);
    if (rgdod == nullptr || pdwInOut == nullptr || cbObjectData < sizeof(DIDEVICEOBJECTDATA) || *pdwInOut == 0)
        return DI_OK;
    const bool hiding = KeysHiding();
    auto at = [&](DWORD i) { return reinterpret_cast<DIDEVICEOBJECTDATA*>(reinterpret_cast<unsigned char*>(rgdod) + (size_t)i * cbObjectData); };
    DWORD n = *pdwInOut, out = 0;
    for (DWORD i = 0; i < n; i++)
    {
        DIDEVICEOBJECTDATA* e = at(i);
        const DWORD k = e->dwOfs & 0xFF;
        const bool down = (e->dwData & 0x80) != 0;
        bool drop = false;
        if (down)
        {
            if (hiding && IsHiddenDik(k))
            {
                g_dikHeld[k] = true;
                drop = true;
            }
        }
        else if (g_dikHeld[k])
        {
            g_dikHeld[k] = false;   // the game never saw it go down
            drop = true;
        }
        if (drop)
        {
            ::InterlockedIncrement(&g_dikHidden);
            continue;
        }
        if (out != i)
            memcpy(at(out), e, cbObjectData);
        out++;
    }
    *pdwInOut = out;
    return DI_OK;
}

static HRESULT __stdcall KeyStateCb(IDirectInputDevice8A* device, DWORD cbData, LPVOID lpvData)
{
    (void)device;
    if (lpvData == nullptr || cbData < 256)
        return DI_OK;
    auto keys = static_cast<uint8_t*>(lpvData);
    const bool hiding = KeysHiding();
    for (DWORD k = 1; k < 256; k++)
    {
        if ((keys[k] & 0x80) == 0)
        {
            g_dikHeld[k] = false;   // released: stop hiding it
            continue;
        }
        if (!g_dikHeld[k] && hiding && IsHiddenDik(k))
            g_dikHeld[k] = true;
        if (g_dikHeld[k])
            keys[k] = 0;
    }
    return DI_OK;
}

// ---------------------------------------------------------------------------------------------
// The game's own window procedure (0.9). Text Services (TSF, e.g. Google Japanese Input) hands some
// messages straight to the game's original window procedure, past the addon's / VanaCore's
// subclass. The game passes them on to DefWindowProcA, which turned the IME's key into a character
// for the game: Space / the confirming Enter then opened its chat line (seen in 0.8's stack dump:
// FFXiMain wndproc -> DefWindowProcA -> ... -> the game's character input -> chat line).
// While the addon's box is active, IME results and characters handed to DefWindowProcA by the game
// are dropped (the addon gets the text through its own subclass); the rest are noted and passed on.
// ---------------------------------------------------------------------------------------------
typedef LRESULT (WINAPI* PFN_DefWP)(HWND, UINT, WPARAM, LPARAM);
static LRESULT WINAPI HookDefWP(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static IatHook g_defwp = {"DefWindowProcA", (void*)&HookDefWP, nullptr, nullptr};
static volatile LONG g_defDropped = 0;

static bool IsKeyOrImeMessage(UINT msg)
{
    return (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || (msg >= WM_IME_STARTCOMPOSITION && msg <= WM_IME_COMPOSITION) ||
           (msg >= WM_IME_SETCONTEXT && msg <= WM_IME_KEYUP);
}

static LRESULT WINAPI HookDefWP(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto real = (PFN_DefWP)g_defwp.real;
    if (g_block && IsKeyOrImeMessage(msg))
    {
        bool drop = false;
        switch (msg)
        {
            case WM_CHAR:
            case WM_DEADCHAR:
            case WM_IME_CHAR:
            case WM_IME_STARTCOMPOSITION:
            case WM_IME_COMPOSITION:
            case WM_IME_ENDCOMPOSITION:
            case WM_IME_KEYDOWN:
            case WM_IME_KEYUP:
                drop = true;
                break;
            default:
                break;
        }
        NoteImm("DefWindowProcA", msg, (DWORD)wp, 0, __builtin_return_address(0), drop);
        if (drop)
        {
            ::InterlockedIncrement(&g_defDropped);
            return 0;
        }
        const UINT prevMsg = g_defMsg;
        const WPARAM prevWp = g_defWp;
        g_defMsg = msg;
        g_defWp  = wp;
        LRESULT r = real ? real(hwnd, msg, wp, lp) : 0;
        g_defMsg = prevMsg;
        g_defWp  = prevWp;
        return r;
    }
    return real ? real(hwnd, msg, wp, lp) : 0;
}

// ---------------------------------------------------------------------------------------------
// The game's own look at each message before it is dispatched (0.91). FFXiMain's message loop
// (PeekMessageA, read once into a register, so an import hook never sees it) copies every MSG it
// takes to a handler (FFXiMain+0x16C0 in this build) before TranslateMessage / DispatchMessageA.
// That handler starts the game's chat input on WM_KEYDOWN VK_PROCESSKEY and on the IME messages
// (WM_IME_STARTCOMPOSITION / WM_IME_COMPOSITION -> FFXiMain+0x1B0840 -> chat line). The call to it
// is redirected here; while the addon's box is active the IME's messages skip the game's handler
// (they are still translated and dispatched by the game's loop, so the IME and the windows get them).
// Found by its code pattern, not by a fixed offset:
//   F3 A5 E8 <rel32> 83 C4 1C 8D 4C 24 14 51 FF 15   (rep movsd; call handler; add esp,1C; ...)
// ---------------------------------------------------------------------------------------------
struct MsgArg { MSG m; };  // the MSG passed by value (7 dwords) to a cdecl handler
typedef void (__cdecl* PFN_Pre)(MsgArg);
static unsigned char* g_preSite = nullptr;   // the E8 of the call
static PFN_Pre        g_preReal = nullptr;
static volatile LONG  g_preDepth = 0;
static volatile LONG  g_preSkipped = 0;

static bool IsImeForGame(const MSG& m)
{
    switch (m.message)
    {
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
            return m.wParam == VK_PROCESSKEY;
        case WM_CHAR:
            return m.wParam >= 0x80;   // DBCS bytes from the IME
        case WM_IME_STARTCOMPOSITION:
        case WM_IME_ENDCOMPOSITION:
        case WM_IME_COMPOSITION:
            return true;
        default:
            return m.message >= WM_IME_SETCONTEXT && m.message <= WM_IME_KEYUP;
    }
}

static void __cdecl HookPre(MsgArg a)
{
    ::InterlockedIncrement(&g_preDepth);
    if (g_block && IsImeForGame(a.m))
    {
        ::InterlockedIncrement(&g_preSkipped);
        NoteImm("game message handler skipped", a.m.message, (DWORD)a.m.wParam, 0, (void*)g_preSite, true);
    }
    else if (g_preReal)
    {
        g_preReal(a);
    }
    ::InterlockedDecrement(&g_preDepth);
}

static bool WriteCode(unsigned char* at, const unsigned char* bytes, size_t n)
{
    DWORD old = 0;
    if (!::VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old))
        return false;
    memcpy(at, bytes, n);
    ::VirtualProtect(at, n, old, &old);
    ::FlushInstructionCache(::GetCurrentProcess(), at, n);
    return true;
}

static void InstallPreHook(HMODULE game)
{
    if (g_preSite || game == nullptr)
        return;
    auto base = reinterpret_cast<unsigned char*>(game);
    auto dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt   = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto sec  = IMAGE_FIRST_SECTION(nt);
    static const int pat[] = {0xF3, 0xA5, 0xE8, -1, -1, -1, -1, 0x83, 0xC4, 0x1C, 0x8D, 0x4C, 0x24, 0x14, 0x51, 0xFF, 0x15};
    const size_t plen = sizeof(pat) / sizeof(pat[0]);
    unsigned char* found = nullptr;
    int matches = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
    {
        if ((sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 || memcmp(sec->Name, ".text", 5) != 0)
            continue;
        unsigned char* p   = base + sec->VirtualAddress;
        unsigned char* end = p + sec->Misc.VirtualSize - plen;
        for (; p < end; p++)
        {
            size_t k = 0;
            while (k < plen && (pat[k] < 0 || p[k] == (unsigned char)pat[k]))
                k++;
            if (k == plen)
            {
                found = p + 2;
                matches++;
            }
        }
    }
    if (matches != 1)
        return;   // not exactly one place: leave the game alone
    const int32_t rel = *reinterpret_cast<int32_t*>(found + 1);
    g_preReal = reinterpret_cast<PFN_Pre>(found + 5 + rel);
    unsigned char call[5] = {0xE8};
    const int32_t nrel = (int32_t)((uintptr_t)&HookPre - (uintptr_t)(found + 5));
    memcpy(call + 1, &nrel, 4);
    if (WriteCode(found, call, 5))
        g_preSite = found;
}

static void RemovePreHook(void)
{
    if (g_preSite == nullptr || g_preReal == nullptr)
        return;
    unsigned char call[5] = {0xE8};
    const int32_t rel = (int32_t)((uintptr_t)g_preReal - (uintptr_t)(g_preSite + 5));
    memcpy(call + 1, &rel, 4);
    WriteCode(g_preSite, call, 5);
    g_preSite = nullptr;
}

static void HookOne(HMODULE game, IatHook& h, const char* dll)
{
    if (h.slot)
        return;
    void** slot = FindIatSlot(game, dll, h.name);
    if (slot && *slot != h.hook)
    {
        h.real = *slot;
        if (WriteSlot(slot, h.hook))
            h.slot = slot;
    }
}

static void UnhookOne(IatHook& h)
{
    if (h.slot && *h.slot == h.hook)
        WriteSlot(h.slot, h.real);
    h.slot = nullptr;
}

static int InstallImmHooks(void)
{
    HMODULE game = ::GetModuleHandleA("FFXiMain.dll");
    HookOne(game, g_async, "USER32.dll");
    HookOne(game, g_kbstate, "USER32.dll");
    HookOne(game, g_defwp, "USER32.dll");
    InstallPreHook(game);
    if (g_peek.slot == nullptr)
    {
        void** slot = FindIatSlot(game, "USER32.dll", "PeekMessageA");
        if (slot && *slot != g_peek.hook)
        {
            g_peek.real = *slot;
            if (WriteSlot(slot, g_peek.hook))
                g_peek.slot = slot;
        }
    }
    int n = 0;
    for (auto& h : g_hooks)
    {
        if (h.slot)
        {
            n++;
            continue;
        }
        void** slot = FindIatSlot(game, "IMM32.dll", h.name);
        if (slot == nullptr || *slot == h.hook)
            continue;
        h.real = *slot;
        if (WriteSlot(slot, h.hook))
        {
            h.slot = slot;
            n++;
        }
    }
    return n;
}

static void RemoveImmHooks(void)
{
    g_block = false;
    UnhookOne(g_async);
    UnhookOne(g_kbstate);
    UnhookOne(g_defwp);
    RemovePreHook();
    if (g_peek.slot && *g_peek.slot == g_peek.hook)
        WriteSlot(g_peek.slot, g_peek.real);
    g_peek.slot = nullptr;
    if (g_peekDepth > 0 || g_preDepth > 0)
    {
        // Unloaded from inside a message this hook dispatched: keep the code in memory.
        HMODULE self = nullptr;
        ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                             reinterpret_cast<LPCWSTR>(&HookPeekA), &self);
    }
    for (auto& h : g_hooks)
    {
        if (h.slot && *h.slot == h.hook)
            WriteSlot(h.slot, h.real);
        h.slot = nullptr;
    }
}


class JapaneseIME final : public IPluginBase
{
public:
    // --- plugin information ---
    const char* GetName(void) const override { return PLUGIN_NAME; }
    const char* GetAuthor(void) const override { return "Tsukasa"; }
    const char* GetDescription(void) const override { return "Japanese IME input for addon text boxes (game chat stays closed)."; }
    const char* GetLink(void) const override { return ""; }
    double GetVersion(void) const override { return 1.0; }
    double GetInterfaceVersion(void) const override { return ASHITA_INTERFACE_VERSION; }
    int32_t GetPriority(void) const override { return 0; }
    uint32_t GetFlags(void) const override
    {
        // UsePluginEvents: without it HandleEvent never gets the addon's events (0.4).
        return (uint32_t)(Ashita::PluginFlags::UseCommands | Ashita::PluginFlags::UseDirect3D | Ashita::PluginFlags::UsePluginEvents);
    }

    // --- lifetime ---
    bool Initialize(IAshitaCore* core, ILogManager* logger, const uint32_t id) override
    {
        (void)id;
        this->m_Log  = logger;
        this->m_Core = core;
        g_instance   = this;
        ::InitializeCriticalSection(&this->m_Lock);
        this->m_LockReady = true;
        if (g_remove_msg == 0)
            g_remove_msg = ::RegisterWindowMessageW(L"japanese_ime.RemoveSubclass");
        this->Print("読み込みました（1.0）。アドオンの入力欄で日本語が使えます。/jime status で状態を表示します。");
        // The game's IME calls are hooked from the start (they pass unchanged until the addon asks).
        this->m_HooksTried = true;
        this->m_HookCount  = InstallImmHooks();
        IKeyboard* kb = (core && core->GetInputManager()) ? core->GetInputManager()->GetKeyboard() : nullptr;
        if (kb)
        {
            kb->AddCallback("__plugin_japanese_ime", &KeyDataCb, &KeyStateCb, nullptr);
            this->m_KeyCb = true;
        }
        {
            char line[192];
            snprintf(line, sizeof(line), "loaded 1.0: imm hooks %d of %d in FFXiMain.dll, game message filter %s, directinput key callbacks %s, key state hooks %s/%s, DefWindowProc hook %s", this->m_HookCount, (int)(sizeof(g_hooks) / sizeof(g_hooks[0])), g_preSite ? "yes" : "NOT FOUND", this->m_KeyCb ? "yes" : "no", g_async.slot ? "yes" : "no", g_kbstate.slot ? "yes" : "no", g_defwp.slot ? "yes" : "no");
            this->Diag(line);
        }
        return true;
    }

    void Release(void) override
    {
        if (this->m_Released)
            return;
        this->m_Released = true;
        this->m_Capture  = false;
        this->m_Manual   = false;
        this->m_Addon    = false;
        g_keysOn         = false;
        g_keysOffAt      = ::GetTickCount() - KEY_GRACE_MS;
        if (this->m_KeyCb && this->m_Core && this->m_Core->GetInputManager() && this->m_Core->GetInputManager()->GetKeyboard())
            this->m_Core->GetInputManager()->GetKeyboard()->RemoveCallback("__plugin_japanese_ime");
        this->m_KeyCb = false;
        this->Unhook(true);
        RemoveImmHooks();
        g_instance = nullptr;
        if (this->m_LockReady)
        {
            ::DeleteCriticalSection(&this->m_Lock);
            this->m_LockReady = false;
        }
    }

    // --- events ---
    // The addon turns capture on while one of its own text boxes is active, and off again (EVENT_CTL).
    // Only flags are set here; the window work happens in Direct3DPresent on the window's thread.
    void HandleEvent(const char* name, const void* data, uint32_t size) override
    {
        if (name == nullptr || strcmp(name, EVENT_CTL) != 0)
            return;
        char v[64] = {};
        if (data != nullptr && size > 0)
            memcpy(v, data, size < sizeof(v) - 1 ? size : sizeof(v) - 1);
        this->Control(v, "event");
    }

    // "on <id>" / "off <id>" (an id is required; "on" alone counts as the id "addon").
    void Control(const char* v, const char* via)
    {
        while (*v == ' ')
            v++;
        bool on;
        if (_strnicmp(v, "on", 2) == 0 && (v[2] == '\0' || v[2] == ' '))
            on = true, v += 2;
        else if (_strnicmp(v, "off", 3) == 0 && (v[3] == '\0' || v[3] == ' '))
            on = false, v += 3;
        else
            return;
        while (*v == ' ')
            v++;
        char id[ID_MAX] = {};
        size_t n = 0;
        while (v[n] && v[n] != ' ' && v[n] != '\t' && v[n] != '\r' && v[n] != '\n' && n < ID_MAX - 1)
        {
            id[n] = v[n];
            n++;
        }
        if (n == 0)
            strcpy(id, "addon");
        this->SetOwner(id, on, via);
    }

    // The addons asking for input, oldest first. The last one that turned on gets the text; when it
    // turns off, the one before it (if still on) gets it again.
    void SetOwner(const char* id, bool on, const char* via)
    {
        char before[ID_MAX] = {};
        char after[ID_MAX]  = {};
        ::EnterCriticalSection(&this->m_Lock);
        if (this->m_OwnerCount > 0)
            strcpy(before, this->m_Owners[this->m_OwnerCount - 1]);
        size_t w = 0;
        for (size_t r = 0; r < this->m_OwnerCount; r++)
        {
            if (strcmp(this->m_Owners[r], id) == 0)
                continue;
            if (w != r)
                strcpy(this->m_Owners[w], this->m_Owners[r]);
            w++;
        }
        this->m_OwnerCount = w;
        if (on)
        {
            if (this->m_OwnerCount == OWNER_MAX)
            {
                memmove(this->m_Owners[0], this->m_Owners[1], sizeof(this->m_Owners[0]) * (OWNER_MAX - 1));
                this->m_OwnerCount--;
            }
            strcpy(this->m_Owners[this->m_OwnerCount++], id);
        }
        if (this->m_OwnerCount > 0)
            strcpy(after, this->m_Owners[this->m_OwnerCount - 1]);
        ::LeaveCriticalSection(&this->m_Lock);
        if (strcmp(before, after) != 0)
        {
            char line[128];
            snprintf(line, sizeof(line), "owner: %s -> %s (%s %s via %s)", before[0] ? before : "-", after[0] ? after : "-", id, on ? "on" : "off", via);
            this->Diag(line);
        }
        this->SetAddon(after[0] != '\0', via);
    }

    void Owner(char (&out)[ID_MAX])
    {
        out[0] = '\0';
        if (!this->m_LockReady)
            return;
        ::EnterCriticalSection(&this->m_Lock);
        if (this->m_OwnerCount > 0)
            strcpy(out, this->m_Owners[this->m_OwnerCount - 1]);
        ::LeaveCriticalSection(&this->m_Lock);
    }

    // From the addon (its plugin event, or the silent command "/jime addon on|off").
    void SetAddon(bool wanted, const char* via)
    {
        this->m_Wanted = wanted;
        const bool on = wanted && !this->m_ChatOpen;
        if (on != this->m_Addon)
        {
            char line[96];
            snprintf(line, sizeof(line), "addon: %s (via %s)", on ? "on, an addon's box is active" : (wanted ? "paused, the game chat is open" : "off"), via);
            this->Diag(line);
            if (on)
            {
                memset(this->m_Seen, 0, sizeof(this->m_Seen));
                g_stackDumps = 0;
            }
            else
                g_keysOffAt = ::GetTickCount();
        }
        this->m_Addon   = on;
        g_keysOn        = on;
        this->m_Capture = this->m_Manual || on;
        this->m_AnswerPending = true;
    }

    bool HandleCommand(int32_t mode, const char* command, bool injected) override
    {
        (void)mode;
        (void)injected;
        if (command == nullptr)
            return false;

        char buf[128] = {};
        strncpy(buf, command, sizeof(buf) - 1);
        char* p = buf;
        while (*p == ' ')
            p++;
        if (_strnicmp(p, "/jime", 5) != 0 || (p[5] != '\0' && p[5] != ' '))
            return false;
        p += 5;
        while (*p == ' ')
            p++;
        char* rest = p;
        for (char* q = p; *q; q++)
        {
            if (*q == ' ')
            {
                *q   = '\0';
                rest = q + 1;
                while (*rest == ' ')
                    rest++;
                break;
            }
            rest = q + 1;
        }

        // The addon's silent switch: no chat line either way.
        if (_stricmp(p, "addon") == 0)
        {
            this->Control(rest, "command");
            return true;
        }

        if (_stricmp(p, "on") == 0)
        {
            ZeroMemory((void*)&this->m_Stats, sizeof(this->m_Stats));
            this->m_Manual  = true;
            this->m_Capture = true;
            this->Print("取得オン：日本語入力をオンにして文字を打ち、確定してみてください。");
        }
        else if (_stricmp(p, "off") == 0)
        {
            this->m_Manual  = false;
            this->m_Capture = this->m_Addon;
            this->Print("取得オフ。");
        }
        else if (_stricmp(p, "diag") == 0)
        {
            this->m_DiagChat = !this->m_DiagChat;
            this->Print(this->m_DiagChat ? "診断の行：チャットにも出します" : "診断の行：ログだけに出します");
        }
        else if (_stricmp(p, "verbose") == 0)
        {
            this->m_Verbose = !this->m_Verbose;
            this->Print(this->m_Verbose ? "詳しい表示：オン（変換中の文字と半角文字も出します）" : "詳しい表示：オフ");
        }
        else if (_stricmp(p, "status") == 0 || *p == '\0')
        {
            this->PrintStatus();
        }
        else
        {
            this->Print("使い方：/jime status | on | off | verbose | diag");
        }
        return true;
    }

    bool HandleIncomingText(int32_t, bool, const char*, int32_t*, bool*, char*, bool, bool) override { return false; }
    bool HandleOutgoingText(int32_t, const char*, int32_t*, char*, bool, bool) override { return false; }
    bool HandleIncomingPacket(uint16_t, uint32_t, const uint8_t*, uint8_t*, uint32_t, const uint8_t*, bool, bool) override { return false; }
    bool HandleOutgoingPacket(uint16_t, uint32_t, const uint8_t*, uint8_t*, uint32_t, const uint8_t*, bool, bool) override { return false; }

    bool Direct3DInitialize(IDirect3DDevice8*) override { return true; }
    void Direct3DBeginScene(bool) override {}
    void Direct3DEndScene(bool) override {}

    void Direct3DPresent(const RECT*, const RECT*, HWND, const RGNDATA*) override
    {
        if (this->m_Released)
            return;
        this->m_RenderThread = ::GetCurrentThreadId();

        // The game's chat line: while it is open, the game types Japanese itself (capture pauses).
        {
            bool chat = false;
            if (this->m_Core && this->m_Core->GetChatManager())
                chat = this->m_Core->GetChatManager()->IsInputOpen() != 0;
            if (chat != this->m_ChatOpen)
            {
                this->m_ChatOpen = chat;
                this->Diag(chat ? "game chat opened" : "game chat closed");
                this->SetAddon(this->m_Wanted, "chat");
            }
        }

        // Hook and unhook from here: the game draws from its window's thread, which is the
        // only thread allowed to subclass that window.
        if (this->m_Capture && !this->m_Hooked)
            this->Hook();
        else if (!this->m_Capture && this->m_Hooked)
            this->Unhook(false);

        // While the addon's box is active, make sure the window has an input context (the game may
        // have taken it away while its chat line is closed), and give the game's back when it ends.
        if (this->m_Addon && this->m_Hooked && this->m_Hwnd && !this->m_AssocTried)
        {
            this->m_AssocTried = true;
            HIMC himc = ::ImmGetContext(this->m_Hwnd);
            if (himc)
                ::ImmReleaseContext(this->m_Hwnd, himc);
            else if (::ImmAssociateContextEx(this->m_Hwnd, nullptr, IACE_DEFAULT))
                this->m_Associated = true;
        }
        else if (!this->m_Addon && this->m_AssocTried)
        {
            this->m_AssocTried = false;
            if (this->m_Associated && this->m_Hwnd && ::IsWindow(this->m_Hwnd))
                ::ImmAssociateContext(this->m_Hwnd, nullptr);
            this->m_Associated = false;
        }

        // The game's IME calls: hooked once capture is first asked for, kept until unload.
        if (this->m_Capture && !this->m_HooksTried)
        {
            this->m_HooksTried = true;
            this->m_HookCount  = InstallImmHooks();
            char line[128];
            snprintf(line, sizeof(line), "imm hooks: %d of %d in FFXiMain.dll, message loop hook %s, key state hooks %s/%s", this->m_HookCount, (int)(sizeof(g_hooks) / sizeof(g_hooks[0])), g_peek.slot ? "yes" : "no", g_async.slot ? "yes" : "no", g_kbstate.slot ? "yes" : "no");
            this->Diag(line);
        }
        g_block = this->m_Addon && this->m_Hooked;
        if (this->m_Hooked && this->m_Hwnd && (this->m_Addon || this->m_Manual))
            this->Monitor();
        else
            this->m_MonValid = false;

        if (this->m_AnswerPending)
        {
            this->m_AnswerPending = false;
            char owner[ID_MAX];
            this->Owner(owner);
            char st[ID_MAX + 8];
            if (this->m_Addon && owner[0])
                snprintf(st, sizeof(st), "on %s", owner);
            else
                strcpy(st, "off");
            this->Raise(EVENT_STATE, st, (uint32_t)strlen(st));
        }

        this->Flush();
        this->FlushDiag();
    }

    bool Direct3DSetRenderState(D3DRENDERSTATETYPE, DWORD*) override { return false; }
    bool Direct3DDrawPrimitive(D3DPRIMITIVETYPE, UINT, UINT) override { return false; }
    bool Direct3DDrawIndexedPrimitive(D3DPRIMITIVETYPE, UINT, UINT, UINT, UINT) override { return false; }
    bool Direct3DDrawPrimitiveUP(D3DPRIMITIVETYPE, UINT, CONST void*, UINT) override { return false; }
    bool Direct3DDrawIndexedPrimitiveUP(D3DPRIMITIVETYPE, UINT, UINT, UINT, CONST void*, D3DFORMAT, CONST void*, UINT) override { return false; }

    // MSVC's scalar deleting destructor slot (see the note at the top).
    virtual void* ScalarDeletingDestructor(unsigned int flags)
    {
        this->Release();
        this->~JapaneseIME();
        if (flags & 1)
            ::HeapFree(::GetProcessHeap(), 0, this);
        return this;
    }

    // --- window hook ---
    static LRESULT CALLBACK SubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
    {
        auto self = reinterpret_cast<JapaneseIME*>(ref);
        if (self != g_instance)
            self = nullptr;

        if (msg == g_remove_msg && g_remove_msg != 0)
        {
            ::RemoveWindowSubclass(hwnd, &JapaneseIME::SubclassProc, id);
            if (self)
                self->m_Hooked = false;
            return 1;
        }
        if (msg == WM_NCDESTROY)
        {
            ::RemoveWindowSubclass(hwnd, &JapaneseIME::SubclassProc, id);
            if (self)
            {
                self->m_Hooked = false;
                self->m_Hwnd   = nullptr;
            }
            return ::DefSubclassProc(hwnd, msg, wp, lp);
        }

        if (self && self->m_Capture)
        {
            self->Observe(hwnd, msg, wp, lp);
            // While the addon's box is active, the composition goes to that box only: the game (its chat
            // line) and the default composition window don't get it. Every other message passes as before.
            if (self->m_Addon)
            {
                switch (msg)
                {
                    // The composition is drawn by the addon; the result goes to it as an event, so
                    // nobody gets WM_IME_CHAR / WM_CHAR for it (the usual way for a program that
                    // draws its own composition).
                    case WM_IME_STARTCOMPOSITION:
                    case WM_IME_COMPOSITION:
                    case WM_IME_ENDCOMPOSITION:
                    case WM_IME_CHAR:
                    case WM_IME_KEYDOWN:   // DefWindowProc would turn these into WM_KEYDOWN / WM_KEYUP for the game
                    case WM_IME_KEYUP:
                        ::InterlockedIncrement(&self->m_Stats.swallowed);
                        return 0;
                    // The IME's own notices go to Windows' default handling (the candidate list and
                    // the mode), not to the game, which closes the IME while its chat line is shut.
                    case WM_IME_NOTIFY:
                    case WM_IME_SETCONTEXT:
                    case WM_IME_SELECT:
                    case WM_IME_CONTROL:
                    case WM_IME_COMPOSITIONFULL:
                    case WM_IME_REQUEST:
                        ::InterlockedIncrement(&self->m_Stats.routed);
                        return ::IsWindowUnicode(hwnd) ? ::DefWindowProcW(hwnd, msg, wp, lp) : ::DefWindowProcA(hwnd, msg, wp, lp);
                    case WM_KEYDOWN:
                    case WM_KEYUP:
                        if (wp == VK_PROCESSKEY)
                        {
                            ::InterlockedIncrement(&self->m_Stats.routed);
                            return ::IsWindowUnicode(hwnd) ? ::DefWindowProcW(hwnd, msg, wp, lp) : ::DefWindowProcA(hwnd, msg, wp, lp);
                        }
                        break;
                    default:
                        break;
                }
            }
        }

        // Observe only: every message still goes on to the game unchanged.
        return ::DefSubclassProc(hwnd, msg, wp, lp);
    }

private:
    IAshitaCore*     m_Core      = nullptr;
    ILogManager*     m_Log       = nullptr;
    bool             m_DiagChat  = false;  // diagnostics go to the log; /jime diag puts them in the chat too
    bool             m_HooksTried = false;
    int              m_HookCount = 0;
    bool             m_MonValid  = false;
    BOOL             m_MonOpen   = FALSE;
    DWORD            m_MonConv   = 0;
    bool             m_MonImc    = false;
    char             m_Diag[16][192] = {};
    size_t           m_DiagLen   = 0;
    char             m_LastCall[160] = {};
    char             m_Seen[32][160] = {};  // recent distinct calls (the game repeats a few in turn on every key)
    size_t           m_SeenNext  = 0;
    LONG             m_LastRepeat = 0;
    CRITICAL_SECTION m_Lock      = {};
    bool             m_LockReady = false;
    volatile bool    m_Capture   = false;  // m_Manual || m_Addon
    volatile bool    m_Manual    = false;  // /jime on
    volatile bool    m_Addon     = false;  // EVENT_CTL "on"
    volatile bool    m_AnswerPending = false;
    volatile bool    m_Wanted    = false;  // some addon has asked (on), even while paused for the game chat
    volatile bool    m_ChatOpen  = false;  // the game's chat line is open
    bool             m_KeyCb     = false;  // DirectInput keyboard callbacks registered
    char             m_Owners[OWNER_MAX][ID_MAX] = {};
    size_t           m_OwnerCount = 0;
    bool             m_AssocTried = false;
    bool             m_Associated = false;
    volatile bool    m_Verbose   = false;
    volatile bool    m_Hooked    = false;
    bool             m_Released  = false;
    bool             m_HookFailedNoted = false;
    HWND             m_Hwnd      = nullptr;
    DWORD            m_RenderThread = 0;
    Stats            m_Stats     = {};
    Item             m_Queue[QUEUE_MAX];
    size_t           m_QueueLen  = 0;

    // Messages ----------------------------------------------------------------------------
    void Observe(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
            case WM_IME_STARTCOMPOSITION:
                ::InterlockedIncrement(&this->m_Stats.start_comp);
                break;
            case WM_IME_ENDCOMPOSITION:
                ::InterlockedIncrement(&this->m_Stats.end_comp);
                this->Push(ItemKind::Comp, L"", 0);
                break;
            case WM_IME_COMPOSITION:
            {
                ::InterlockedIncrement(&this->m_Stats.composition);
                HIMC himc = ::ImmGetContext(hwnd);
                if (himc == nullptr)
                    break;
                if (lp & GCS_RESULTSTR)
                {
                    wchar_t text[TEXT_MAX];
                    uint32_t n = ReadComp(himc, GCS_RESULTSTR, text);
                    if (n > 0)
                    {
                        ::InterlockedIncrement(&this->m_Stats.result);
                        this->Push(ItemKind::Result, text, n);
                    }
                }
                if (lp & GCS_COMPSTR)
                {
                    wchar_t text[TEXT_MAX];
                    uint32_t n = ReadComp(himc, GCS_COMPSTR, text);
                    this->Push(ItemKind::Comp, text, n);
                }
                ::ImmReleaseContext(hwnd, himc);
                break;
            }
            case WM_IME_CHAR:
                ::InterlockedIncrement(&this->m_Stats.ime_char);
                break;
            case WM_IME_NOTIFY:
                ::InterlockedIncrement(&this->m_Stats.ime_notify);
                if (wp == IMN_SETOPENSTATUS || wp == IMN_SETCONVERSIONMODE)
                {
                    HIMC himc = ::ImmGetContext(hwnd);
                    DWORD conv = 0, sent = 0;
                    BOOL open = FALSE;
                    if (himc)
                    {
                        open = ::ImmGetOpenStatus(himc);
                        ::ImmGetConversionStatus(himc, &conv, &sent);
                        ::ImmReleaseContext(hwnd, himc);
                    }
                    char line[160];
                    snprintf(line, sizeof(line), "notify %s: open=%d conv=0x%lx imc=%s addon=%s",
                             wp == IMN_SETOPENSTATUS ? "IMN_SETOPENSTATUS" : "IMN_SETCONVERSIONMODE",
                             (int)open, (unsigned long)conv, himc ? "yes" : "no", this->m_Addon ? "on" : "off");
                    this->Diag(line);
                }
                break;
            case WM_IME_SETCONTEXT:
                ::InterlockedIncrement(&this->m_Stats.ime_setcontext);
                break;
            case WM_CHAR:
            {
                ::InterlockedIncrement(&this->m_Stats.wm_char);
                if (this->m_Verbose && wp >= 0x20 && wp != 0x7F)
                {
                    wchar_t c = (wchar_t)wp;
                    this->Push(ItemKind::Char, &c, 1);
                }
                break;
            }
            default:
                break;
        }
    }

    static uint32_t ReadComp(HIMC himc, DWORD which, wchar_t (&out)[TEXT_MAX])
    {
        LONG bytes = ::ImmGetCompositionStringW(himc, which, nullptr, 0);
        if (bytes <= 0)
            return 0;
        if ((size_t)bytes > (TEXT_MAX - 1) * sizeof(wchar_t))
            bytes = (LONG)((TEXT_MAX - 1) * sizeof(wchar_t));
        LONG got = ::ImmGetCompositionStringW(himc, which, out, (DWORD)bytes);
        if (got <= 0)
            return 0;
        uint32_t n = (uint32_t)(got / sizeof(wchar_t));
        out[n] = L'\0';
        return n;
    }

    void Push(ItemKind kind, const wchar_t* text, uint32_t len)
    {
        if (!this->m_LockReady)
            return;
        ::EnterCriticalSection(&this->m_Lock);
        if (this->m_QueueLen < QUEUE_MAX)
        {
            Item& it = this->m_Queue[this->m_QueueLen++];
            it.kind  = kind;
            it.len   = len < TEXT_MAX - 1 ? len : (uint32_t)(TEXT_MAX - 1);
            if (it.len)
                memcpy(it.text, text, it.len * sizeof(wchar_t));
            it.text[it.len] = L'\0';
        }
        ::LeaveCriticalSection(&this->m_Lock);
    }

    void Flush(void)
    {
        if (!this->m_LockReady || this->m_QueueLen == 0)
            return;

        // Copy out under the lock, then print and raise without it.
        static Item local[QUEUE_MAX];
        size_t count = 0;
        ::EnterCriticalSection(&this->m_Lock);
        count = this->m_QueueLen;
        memcpy(local, this->m_Queue, count * sizeof(Item));
        this->m_QueueLen = 0;
        ::LeaveCriticalSection(&this->m_Lock);

        for (size_t i = 0; i < count; i++)
        {
            const Item& it = local[i];
            char utf8[TEXT_MAX * 4];
            int u = ::WideCharToMultiByte(CP_UTF8, 0, it.text, (int)it.len, utf8, (int)sizeof(utf8) - 1, nullptr, nullptr);
            if (u < 0)
                u = 0;
            utf8[u] = '\0';
            char owner[ID_MAX];
            this->Owner(owner);
            char tagged[ID_MAX + TEXT_MAX * 4 + 2];
            int t = snprintf(tagged, sizeof(tagged), "%s\t%s", owner, utf8);
            if (t < 0)
                t = 0;
            if (t >= (int)sizeof(tagged))
                t = (int)sizeof(tagged) - 1;

            switch (it.kind)
            {
                case ItemKind::Result:
                {
                    this->Raise(EVENT_RESULT, tagged, (uint32_t)t);
                    if (this->m_Manual)
                    {
                        char line[TEXT_MAX * 4 + 64];
                        snprintf(line, sizeof(line), "確定: %s", utf8);
                        this->Print(line);
                    }
                    break;
                }
                case ItemKind::Comp:
                {
                    this->Raise(EVENT_COMP, tagged, (uint32_t)t);
                    if (this->m_Verbose && it.len > 0)
                    {
                        char line[TEXT_MAX * 4 + 64];
                        snprintf(line, sizeof(line), "変換中: %s", utf8);
                        this->Print(line);
                    }
                    break;
                }
                case ItemKind::Char:
                {
                    char line[96];
                    snprintf(line, sizeof(line), "文字: %s", utf8);
                    this->Print(line);
                    break;
                }
                default:
                    break;
            }
        }
    }

    void Raise(const char* name, const char* data, uint32_t size)
    {
        if (this->m_Core == nullptr)
            return;
        auto pm = this->m_Core->GetPluginManager();
        if (pm)
            pm->RaiseEvent(name, data, size);
    }

    // Hooking -----------------------------------------------------------------------------
    struct FindCtx
    {
        DWORD pid;
        HWND  found;
    };

    static BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lp)
    {
        auto ctx  = reinterpret_cast<FindCtx*>(lp);
        DWORD pid = 0;
        ::GetWindowThreadProcessId(hwnd, &pid);
        if (pid != ctx->pid)
            return TRUE;
        char cls[64] = {};
        ::GetClassNameA(hwnd, cls, sizeof(cls));
        if (_stricmp(cls, "FFXiClass") == 0)
        {
            ctx->found = hwnd;
            return FALSE;
        }
        return TRUE;
    }

    HWND FindGameWindow(void)
    {
        HWND hwnd = nullptr;
        if (this->m_Core && this->m_Core->GetProperties())
            hwnd = this->m_Core->GetProperties()->GetFinalFantasyHwnd();
        if (hwnd && ::IsWindow(hwnd))
            return hwnd;
        FindCtx ctx = {::GetCurrentProcessId(), nullptr};
        ::EnumWindows(&JapaneseIME::EnumProc, (LPARAM)&ctx);
        return ctx.found;
    }

    void Hook(void)
    {
        HWND hwnd = this->FindGameWindow();
        if (hwnd == nullptr)
        {
            this->NoteHookFailure("ゲームのウィンドウが見つかりませんでした。");
            return;
        }
        if (::GetWindowThreadProcessId(hwnd, nullptr) != ::GetCurrentThreadId())
        {
            this->NoteHookFailure("ウィンドウと描画のスレッドが違うため、取得を始められませんでした。");
            return;
        }
        if (!::SetWindowSubclass(hwnd, &JapaneseIME::SubclassProc, SUBCLASS_ID, (DWORD_PTR)this))
        {
            this->NoteHookFailure("ウィンドウにつなげられませんでした。");
            return;
        }
        this->m_Hwnd   = hwnd;
        this->m_Hooked = true;
        this->m_HookFailedNoted = false;
    }

    void NoteHookFailure(const char* why)
    {
        if (this->m_HookFailedNoted)
            return;
        this->m_HookFailedNoted = true;
        this->Print(why);
    }

    void Unhook(bool unloading)
    {
        if (!this->m_Hooked || this->m_Hwnd == nullptr)
        {
            this->m_Hooked = false;
            return;
        }
        HWND hwnd = this->m_Hwnd;
        if (::GetWindowThreadProcessId(hwnd, nullptr) == ::GetCurrentThreadId())
        {
            ::RemoveWindowSubclass(hwnd, &JapaneseIME::SubclassProc, SUBCLASS_ID);
            this->m_Hooked = false;
        }
        else if (g_remove_msg != 0)
        {
            DWORD_PTR res = 0;
            ::SendMessageTimeoutW(hwnd, g_remove_msg, 0, 0, SMTO_ABORTIFHUNG, 2000, &res);
        }

        if (this->m_Hooked && unloading)
        {
            // Could not detach: keep this DLL in memory so the window never calls into freed code.
            HMODULE self = nullptr;
            ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                 reinterpret_cast<LPCWSTR>(&JapaneseIME::SubclassProc), &self);
            this->m_Capture = false;
        }
        this->m_Hooked = false;
        this->m_Hwnd   = nullptr;
    }

public:
    // Diagnostics -------------------------------------------------------------------------
    // Queued (they can come from inside the game's IME code) and written from Direct3DPresent.
    void Diag(const char* text)
    {
        if (!this->m_LockReady)
            return;
        ::EnterCriticalSection(&this->m_Lock);
        if (this->m_DiagLen < 16)
        {
            strncpy(this->m_Diag[this->m_DiagLen], text, sizeof(this->m_Diag[0]) - 1);
            this->m_Diag[this->m_DiagLen][sizeof(this->m_Diag[0]) - 1] = '\0';
            this->m_DiagLen++;
        }
        ::LeaveCriticalSection(&this->m_Lock);
    }

    void NoteCall(const char* fn, DWORD a, DWORD b, DWORD c, void* ret, bool blocked)
    {
        ::InterlockedIncrement(&g_calls);
        char where[64] = "?";
        HMODULE mod = nullptr;
        if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ret, &mod) && mod)
        {
            char path[MAX_PATH] = {};
            ::GetModuleFileNameA(mod, path, MAX_PATH);
            const char* name = strrchr(path, '\\');
            name = name ? name + 1 : path;
            snprintf(where, sizeof(where), "%s+0x%lx", name, (unsigned long)((uintptr_t)ret - (uintptr_t)mod));
        }
        char line[160];
        snprintf(line, sizeof(line), "call %s(%lu, %lu, %lu) from %s%s addon=%s", fn, (unsigned long)a, (unsigned long)b,
                 (unsigned long)c, where, blocked ? " BLOCKED" : "", this->m_Addon ? "on" : "off");
        strncpy(this->m_LastCall, line, sizeof(this->m_LastCall) - 1);
        // Each distinct call once (until it drops out of the last 32); the counts are in /jime status.
        for (auto& seen : this->m_Seen)
        {
            if (strcmp(seen, line) == 0)
            {
                this->m_LastRepeat++;
                return;
            }
        }
        strncpy(this->m_Seen[this->m_SeenNext], line, sizeof(this->m_Seen[0]) - 1);
        this->m_SeenNext = (this->m_SeenNext + 1) % 32;
        this->Diag(line);
    }

private:
    void FlushDiag(void)
    {
        if (!this->m_LockReady || this->m_DiagLen == 0)
            return;
        static char local[16][192];
        size_t count = 0;
        ::EnterCriticalSection(&this->m_Lock);
        count = this->m_DiagLen;
        memcpy(local, this->m_Diag, sizeof(local));
        this->m_DiagLen = 0;
        ::LeaveCriticalSection(&this->m_Lock);
        for (size_t i = 0; i < count; i++)
        {
            if (this->m_Log)
                this->m_Log->Log(4, PLUGIN_NAME, local[i]);
            if (this->m_DiagChat)
            {
                char line[220];
                snprintf(line, sizeof(line), "diag: %s", local[i]);
                this->PrintRaw(line);
            }
        }
    }

    // The window's IME state, compared frame to frame while capture is on.
    LONG m_LoopHiddenSeen = 0;
    void Monitor(void)
    {
        const LONG hidden = g_loopHidden;
        if (hidden != this->m_LoopHiddenSeen)
        {
            char hl[128];
            snprintf(hl, sizeof(hl), "loop: %ld IME key/messages kept from the game's message loop (total %ld)",
                     hidden - this->m_LoopHiddenSeen, hidden);
            this->m_LoopHiddenSeen = hidden;
            this->Diag(hl);
        }
        HIMC himc = ::ImmGetContext(this->m_Hwnd);
        BOOL open = FALSE;
        DWORD conv = 0, sent = 0;
        if (himc)
        {
            open = ::ImmGetOpenStatus(himc);
            ::ImmGetConversionStatus(himc, &conv, &sent);
            ::ImmReleaseContext(this->m_Hwnd, himc);
        }
        const bool imc = himc != nullptr;
        if (!this->m_MonValid || open != this->m_MonOpen || conv != this->m_MonConv || imc != this->m_MonImc)
        {
            char line[192];
            snprintf(line, sizeof(line), "state: open=%d conv=0x%lx imc=%s (was open=%d conv=0x%lx imc=%s) addon=%s last=[%s]",
                     (int)open, (unsigned long)conv, imc ? "yes" : "no",
                     (int)this->m_MonOpen, (unsigned long)this->m_MonConv, this->m_MonImc ? "yes" : "no",
                     this->m_Addon ? "on" : "off", this->m_LastCall[0] ? this->m_LastCall + 5 : "-");
            if (this->m_MonValid)
                this->Diag(line);
            else
            {
                snprintf(line, sizeof(line), "state: open=%d conv=0x%lx imc=%s addon=%s", (int)open, (unsigned long)conv, imc ? "yes" : "no", this->m_Addon ? "on" : "off");
                this->Diag(line);
            }
            this->m_MonValid = true;
            this->m_MonOpen  = open;
            this->m_MonConv  = conv;
            this->m_MonImc   = imc;
        }
    }

    // Chat --------------------------------------------------------------------------------
    static void ToSjis(const wchar_t* text, uint32_t len, char* out, size_t outSize)
    {
        int n = ::WideCharToMultiByte(932, 0, text, (int)len, out, (int)outSize - 1, nullptr, nullptr);
        if (n < 0)
            n = 0;
        out[n] = '\0';
    }

    // Prints a UTF-8 literal from this source file.
    void Print(const char* utf8)
    {
        wchar_t wide[512];
        int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, 511);
        if (n <= 0)
            return;
        char sjis[1024];
        ToSjis(wide, (uint32_t)(n - 1), sjis, sizeof(sjis));
        this->PrintRaw(sjis);
    }

    // Prints Shift-JIS text.
    void PrintRaw(const char* sjis)
    {
        if (this->m_Core == nullptr || this->m_Core->GetChatManager() == nullptr)
            return;
        char line[1200];
        snprintf(line, sizeof(line), "[japanese_ime] %s", sjis);
        this->m_Core->GetChatManager()->Write(CHAT_MODE, false, line);
    }

    void PrintStatus(void)
    {
        char line[512];
        HIMC himc  = nullptr;
        BOOL open  = FALSE;
        DWORD conv = 0, sent = 0;
        bool hasCtx = false;
        if (this->m_Hwnd && this->m_RenderThread == ::GetCurrentThreadId())
        {
            himc = ::ImmGetContext(this->m_Hwnd);
            if (himc)
            {
                hasCtx = true;
                open   = ::ImmGetOpenStatus(himc);
                ::ImmGetConversionStatus(himc, &conv, &sent);
                ::ImmReleaseContext(this->m_Hwnd, himc);
            }
        }
        HWND hwnd = this->m_Hwnd ? this->m_Hwnd : this->FindGameWindow();
        DWORD wt  = hwnd ? ::GetWindowThreadProcessId(hwnd, nullptr) : 0;

        {
            char owner[ID_MAX];
            this->Owner(owner);
            snprintf(line, sizeof(line), "japanese_ime 1.0: owner=%s owners=%u chat_open=%s di_callbacks=%s di_keys_hidden=%ld di_calls=%ld",
                     owner[0] ? owner : "-", (unsigned)this->m_OwnerCount, this->m_ChatOpen ? "yes" : "no", this->m_KeyCb ? "yes" : "no", g_dikHidden, g_dikCalls);
            this->PrintRaw(line);
        }
        snprintf(line, sizeof(line), "addon=%s assoc=%s swallowed=%ld routed=%ld imm_hooks=%d calls=%ld blocked=%ld loop_hook=%s loop_hidden=%ld key_hooks=%s/%s keys_hidden=%ld defwp_hook=%s defwp_dropped=%ld msg_filter=%s msg_skipped=%ld",
                 this->m_Addon ? "on" : "off", this->m_Associated ? "yes" : "no", this->m_Stats.swallowed, this->m_Stats.routed,
                 this->m_HookCount, g_calls, g_blocked, g_peek.slot ? "yes" : "no", g_loopHidden, g_async.slot ? "yes" : "no", g_kbstate.slot ? "yes" : "no", g_keysHidden, g_defwp.slot ? "yes" : "no", g_defDropped, g_preSite ? "yes" : "no", g_preSkipped);
        this->PrintRaw(line);
        snprintf(line, sizeof(line), "capture=%s hooked=%s hwnd=%p win_thread=%lu render_thread=%lu cmd_thread=%lu imc=%s ime_open=%s conv=0x%lx sent=0x%lx",
                 this->m_Capture ? "on" : "off", this->m_Hooked ? "yes" : "no", (void*)hwnd,
                 (unsigned long)wt, (unsigned long)this->m_RenderThread, (unsigned long)::GetCurrentThreadId(),
                 hasCtx ? "yes" : (this->m_Hwnd ? "no" : "?"), hasCtx ? (open ? "yes" : "no") : "?",
                 (unsigned long)conv, (unsigned long)sent);
        this->PrintRaw(line);
        snprintf(line, sizeof(line), "WM_IME: start=%ld comp=%ld result=%ld end=%ld char=%ld notify=%ld setcontext=%ld / WM_CHAR=%ld",
                 this->m_Stats.start_comp, this->m_Stats.composition, this->m_Stats.result, this->m_Stats.end_comp,
                 this->m_Stats.ime_char, this->m_Stats.ime_notify, this->m_Stats.ime_setcontext, this->m_Stats.wm_char);
        this->PrintRaw(line);
    }
};

namespace
{
    void NoteText(const char* text)
    {
        JapaneseIME* self = g_instance;
        if (self)
            self->Diag(text);
    }

    void NoteImm(const char* fn, DWORD a, DWORD b, DWORD c, void* ret, bool blocked)
    {
        JapaneseIME* self = g_instance;
        if (self)
            self->NoteCall(fn, a, b, c, ret, blocked);
    }
}

// ---------------------------------------------------------------------------------------------
// Exports (__stdcall, undecorated names through japanese_ime.def)
// ---------------------------------------------------------------------------------------------

extern "C" IPlugin* __stdcall expCreatePlugin(const char* args)
{
    (void)args;
    void* mem = ::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(JapaneseIME));
    if (mem == nullptr)
        return nullptr;
    auto plugin = new (mem) JapaneseIME();
    return reinterpret_cast<IPlugin*>(static_cast<IPluginBase*>(plugin));
}

extern "C" double __stdcall expGetInterfaceVersion(void)
{
    return ASHITA_INTERFACE_VERSION;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID)
{
    return TRUE;
}
