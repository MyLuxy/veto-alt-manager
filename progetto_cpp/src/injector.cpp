// VetoAltManager — Dark-theme GUI injector

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")
using namespace Gdiplus;

// ── Palette ────────────────────────────────────────────────────────────────
#define C_BG       RGB( 11, 11, 20)   // deepest background
#define C_BG2      RGB( 20, 20, 34)   // control fill
#define C_BG3      RGB( 32, 32, 52)   // border / separator
#define C_ACCENT   RGB(124, 58,237)   // purple
#define C_ACCENTHI RGB(167,139,250)   // light purple
#define C_TEXT     RGB(241,245,249)
#define C_DIM      RGB(148,163,184)
#define C_GREEN    RGB( 34,197, 94)   // [Vanilla]
#define C_BLUE     RGB( 59,130,246)   // [OptiFine]
#define C_ORANGE   RGB(249,115, 22)   // [Lunar Client]
#define C_RED      RGB(239, 68, 68)   // [Badlion]

// ── IDs ─────────────────────────────────────────────────────────────────────
#define IDC_LIST    101
#define IDC_EDIT    102
#define IDC_INJECT  104
#define IDC_STATUS  105
#define IDC_CHANGE  108
#define IDC_RESTORE 109
#define IDT_REFRESH 200
#define IDT_SPLASH  201

#define PIPE_NAME "\\\\.\\pipe\\VetoAltMgr"

// ── Globals ──────────────────────────────────────────────────────────────────
HINSTANCE  g_hInst;
HWND       g_hMain   = NULL;
HWND       g_hSplash = NULL;
HWND       g_hList, g_hEdit, g_hStatus;
HWND       g_hBtnInject, g_hBtnChange, g_hBtnRestore;

std::vector<DWORD>       g_pids;
std::vector<std::string> g_names;

ULONG_PTR  g_gdip = 0;
Image*     g_logo = NULL;      // loaded from logo.png

HBRUSH g_brBG  = NULL;
HBRUSH g_brBG2 = NULL;
HBRUSH g_brBG3 = NULL;

HFONT  g_fntTitle = NULL;   // 20pt bold  (splash title)
HFONT  g_fntSub   = NULL;   // 10pt       (splash subtitle)
HFONT  g_fntHead  = NULL;   // 14pt bold  (main header)
HFONT  g_fntNorm  = NULL;   // 11pt
HFONT  g_fntSmall = NULL;   // 9pt

// Splash animation
int    g_prog10   = 0;       // progress × 10  (0–1000)
bool   g_scanned  = false;   // whether we did the process scan already

// ── Forward declarations ────────────────────────────────────────────────────
LRESULT CALLBACK WndProc   (HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK SplashProc(HWND, UINT, WPARAM, LPARAM);
void RefreshProcessList(HWND, bool);
bool InjectDLL(DWORD, const char*);
bool SendPipeCommand(const char*, char*, int);

// ── Helpers ──────────────────────────────────────────────────────────────────
static HFONT MakeFont(int pt, bool bold, const char* face = "Segoe UI") {
    HDC dc = GetDC(NULL);
    int h = -MulDiv(pt, GetDeviceCaps(dc, LOGPIXELSY), 72);
    ReleaseDC(NULL, dc);
    return CreateFontA(h, 0,0,0, bold ? FW_BOLD : FW_NORMAL,
        0,0,0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH|FF_DONTCARE, face);
}

static void ApplyDarkFrame(HWND hwnd) {
    BOOL d = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &d, sizeof(d)); // DWMWA_USE_IMMERSIVE_DARK_MODE
    DwmSetWindowAttribute(hwnd, 19, &d, sizeof(d)); // pre-release alias
    DWORD pref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, 33, &pref, sizeof(pref)); // rounded corners (Win11)
}

static void DrawRoundRect(Graphics& g, int x, int y, int w, int h, int r,
                           Color fill, Color border) {
    int d = r * 2;
    GraphicsPath p;
    p.AddArc(x,         y,         d, d, 180, 90);
    p.AddArc(x+w-1-d,   y,         d, d, 270, 90);
    p.AddArc(x+w-1-d,   y+h-1-d,   d, d,   0, 90);
    p.AddArc(x,         y+h-1-d,   d, d,  90, 90);
    p.CloseFigure();
    SolidBrush sb(fill);
    Pen        pn(border, 1.f);
    g.FillPath(&sb, &p);
    g.DrawPath(&pn, &p);
}

static void DrawTextCentered(HDC hdc, HFONT fnt, const char* txt, RECT r,
                              COLORREF clr) {
    HFONT old = (HFONT)SelectObject(hdc, fnt);
    SetTextColor(hdc, clr);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextA(hdc, txt, -1, &r, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    SelectObject(hdc, old);
}

static std::string GetLogoPath() {
    char buf[MAX_PATH];
    GetModuleFileNameA(NULL, buf, MAX_PATH);
    char* s = strrchr(buf, '\\'); if (s) *s = '\0';
    return std::string(buf) + "\\logo.png";
}

static const char* SplashStageText(int prog) {
    if (prog <  25) return "Initializing...";
    if (prog <  55) return "Loading modules...";
    if (prog <  85) return "Scanning processes...";
    if (prog < 100) return "Almost ready...";
    return "Done!";
}

// ── Client-type detection ────────────────────────────────────────────────────
struct WinTitleCtx { DWORD pid; std::string title; };
static BOOL CALLBACK FindWinTitle(HWND hwnd, LPARAM lp) {
    auto* c = (WinTitleCtx*)lp;
    DWORD p = 0; GetWindowThreadProcessId(hwnd, &p);
    if (p != c->pid || !IsWindowVisible(hwnd)) return TRUE;
    char buf[512];
    if (GetWindowTextA(hwnd, buf, 512) > 4) { c->title = buf; return FALSE; }
    return TRUE;
}

static std::string GetClientLabel(DWORD pid) {
    HANDLE hp = OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ, FALSE, pid);
    if (hp) {
        char path[MAX_PATH]{}; DWORD sz = MAX_PATH;
        QueryFullProcessImageNameA(hp, 0, path, &sz);
        _strlwr(path); CloseHandle(hp);
        if (strstr(path,"lunar")||strstr(path,"moonsworth")) return "[Lunar Client]";
        if (strstr(path,"badlion")||strstr(path,"bac-"))      return "[Badlion]";
    }
    WinTitleCtx ctx{pid,""};
    EnumWindows(FindWinTitle,(LPARAM)&ctx);
    if (!ctx.title.empty()) {
        std::string t = ctx.title;
        for (auto& c : t) c = (char)tolower((unsigned char)c);
        if (strstr(t.c_str(),"lunar"))    return "[Lunar Client]";
        if (strstr(t.c_str(),"badlion"))  return "[Badlion]";
        if (strstr(t.c_str(),"optifine")) return "[OptiFine]";
        if (strstr(t.c_str(),"minecraft"))return "[Vanilla]";
    }
    HANDLE hp2 = OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ, FALSE, pid);
    if (hp2) {
        HMODULE mods[512]; DWORD need=0;
        if (EnumProcessModules(hp2,mods,sizeof(mods),&need)) {
            DWORD n = (need/sizeof(HMODULE))<512?(need/sizeof(HMODULE)):512;
            for (DWORD m=0;m<n;m++) {
                char nm[MAX_PATH]{}; GetModuleBaseNameA(hp2,mods[m],nm,MAX_PATH);
                _strlwr(nm);
                if (strstr(nm,"lwjgl")||strstr(nm,"openal")||strstr(nm,"jinput"))
                    { CloseHandle(hp2); return "[Vanilla]"; }
            }
        }
        CloseHandle(hp2);
    }
    return "";
}

static COLORREF TagColor(const char* label) {
    if (strstr(label,"Vanilla"))      return C_GREEN;
    if (strstr(label,"OptiFine"))     return C_BLUE;
    if (strstr(label,"Lunar"))        return C_ORANGE;
    if (strstr(label,"Badlion"))      return C_RED;
    return C_DIM;
}

// ════════════════════════════════════════════════════════════════════════════
// SPLASH WINDOW
// ════════════════════════════════════════════════════════════════════════════
LRESULT CALLBACK SplashProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, IDT_SPLASH, 16, NULL);
        break;

    case WM_TIMER: {
        if (wp != IDT_SPLASH) break;
        // Ease-in-out progression
        if      (g_prog10 <  300) g_prog10 += 18;
        else if (g_prog10 <  650) g_prog10 += 12;
        else if (g_prog10 <  900) g_prog10 +=  6;
        else                      g_prog10 += 15;
        if (g_prog10 > 1000) g_prog10 = 1000;
        int prog = g_prog10 / 10;

        // Do the real scan at ~85%
        if (prog >= 85 && !g_scanned && g_hMain) {
            g_scanned = true;
            RefreshProcessList(g_hMain, true);
        }

        InvalidateRect(hwnd, NULL, FALSE);

        if (prog >= 100) {
            KillTimer(hwnd, IDT_SPLASH);
            Sleep(280);
            ShowWindow(g_hMain, SW_SHOW);
            UpdateWindow(g_hMain);
            DestroyWindow(hwnd);
        }
        break;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT cr; GetClientRect(hwnd, &cr);
        int W = cr.right, H = cr.bottom;
        int prog = g_prog10 / 10;

        // ── Double-buffer ──
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
        SelectObject(mem, bmp);

        // Background
        HBRUSH bgBr = CreateSolidBrush(C_BG);
        FillRect(mem, &cr, bgBr); DeleteObject(bgBr);

        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        // ── Logo ──
        int logoSz = 120;
        int logoX  = (W - logoSz) / 2;
        int logoY  = 38;
        if (g_logo && g_logo->GetLastStatus() == Ok) {
            g.DrawImage(g_logo, logoX, logoY, logoSz, logoSz);
        } else {
            // Placeholder: colored rounded square
            DrawRoundRect(g, logoX, logoY, logoSz, logoSz, 14,
                Color(255,124,58,237), Color(255,167,139,250));
            // "V" letter
            HFONT oldF = (HFONT)SelectObject(mem, g_fntTitle);
            SetTextColor(mem, C_TEXT); SetBkMode(mem, TRANSPARENT);
            RECT vr = {logoX, logoY, logoX+logoSz, logoY+logoSz};
            DrawTextA(mem, "V", -1, &vr, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            SelectObject(mem, oldF);
        }

        // ── Title ──
        DrawTextCentered(mem, g_fntTitle, "VetoAltManager",
            {0, logoY+logoSz+12, W, logoY+logoSz+44}, C_TEXT);

        // ── Subtitle ──
        DrawTextCentered(mem, g_fntSub, "1.8.9 Alt Manager",
            {0, logoY+logoSz+46, W, logoY+logoSz+66}, C_DIM);

        // ── Progress bar track ──
        int barX = 40, barW = W - 80, barH = 7, barY = H - 46;
        DrawRoundRect(g, barX, barY, barW, barH, 3,
            Color(255,32,32,52), Color(0,0,0,0));

        // ── Progress bar fill ──
        int fillW = (barW * prog) / 100;
        if (fillW > 6) {
            LinearGradientBrush lgb(
                PointF((float)barX, 0), PointF((float)(barX+fillW), 0),
                Color(255,124,58,237), Color(255,167,139,250));
            GraphicsPath fp;
            fp.AddArc(barX,         barY,         6,barH, 180, 90);
            fp.AddArc(barX+fillW-6, barY,         6,barH, 270, 90);
            fp.AddArc(barX+fillW-6, barY+barH-6,  6,barH,   0, 90);
            fp.AddArc(barX,         barY+barH-6,  6,barH,  90, 90);
            fp.CloseFigure();
            g.FillPath(&lgb, &fp);
        }

        // ── Loading text ──
        DrawTextCentered(mem, g_fntSub, SplashStageText(prog),
            {0, H-30, W, H-12}, C_DIM);

        BitBlt(hdc, 0,0,W,H, mem,0,0, SRCCOPY);
        DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hwnd, &ps);
        break;
    }

    case WM_DESTROY:
        g_hSplash = NULL;
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

// ════════════════════════════════════════════════════════════════════════════
// MAIN WINDOW
// ════════════════════════════════════════════════════════════════════════════
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {

    case WM_CREATE: {
        ApplyDarkFrame(hWnd);

        // ── Controls ──
        // Username row (y=78)
        CreateWindowA("STATIC","Username:", WS_VISIBLE|WS_CHILD,
            15,82,78,20, hWnd,NULL,g_hInst,NULL);
        g_hEdit = CreateWindowA("EDIT","",
            WS_VISIBLE|WS_CHILD|ES_LEFT,
            98,78,195,24, hWnd,(HMENU)IDC_EDIT,g_hInst,NULL);
        SendMessage(g_hEdit, EM_LIMITTEXT, 32, 0);

        // Status (y=114)
        g_hStatus = CreateWindowA("STATIC",
            "Ready. Select a process and enter a username.",
            WS_VISIBLE|WS_CHILD|SS_CENTERIMAGE|SS_PATHELLIPSIS,
            15,112,490,22, hWnd,(HMENU)IDC_STATUS,g_hInst,NULL);

        // List label (y=148)
        CreateWindowA("STATIC","Minecraft Processes  (auto-refresh)",
            WS_VISIBLE|WS_CHILD,
            15,148,300,18, hWnd,NULL,g_hInst,NULL);

        // Process listbox — owner-draw (y=168)
        g_hList = CreateWindowA("LISTBOX","",
            WS_VISIBLE|WS_CHILD|WS_BORDER|WS_VSCROLL|
            LBS_NOTIFY|LBS_OWNERDRAWFIXED|LBS_HASSTRINGS|LBS_NOINTEGRALHEIGHT,
            15,168,490,195, hWnd,(HMENU)IDC_LIST,g_hInst,NULL);

        // Buttons (y=376)
        g_hBtnInject  = CreateWindowA("BUTTON","Inject DLL",
            WS_VISIBLE|WS_CHILD|BS_OWNERDRAW,
            15,376,112,32, hWnd,(HMENU)IDC_INJECT,g_hInst,NULL);
        g_hBtnChange  = CreateWindowA("BUTTON","Change Alt",
            WS_VISIBLE|WS_CHILD|BS_OWNERDRAW,
            137,376,132,32, hWnd,(HMENU)IDC_CHANGE,g_hInst,NULL);
        g_hBtnRestore = CreateWindowA("BUTTON","Restore Original",
            WS_VISIBLE|WS_CHILD|BS_OWNERDRAW,
            279,376,148,32, hWnd,(HMENU)IDC_RESTORE,g_hInst,NULL);

        // Apply fonts
        auto setFont = [](HWND c, HFONT f){ SendMessage(c,WM_SETFONT,(WPARAM)f,TRUE); };
        setFont(g_hEdit,    g_fntNorm);
        setFont(g_hStatus,  g_fntSmall);
        setFont(g_hList,    g_fntNorm);

        // Set fonts on all static labels
        EnumChildWindows(hWnd, [](HWND c, LPARAM lp) -> BOOL {
            char cls[32]{}; GetClassNameA(c,cls,32);
            if (_stricmp(cls,"STATIC")==0)
                SendMessage(c,WM_SETFONT,(WPARAM)(HFONT)lp,TRUE);
            return TRUE;
        }, (LPARAM)g_fntSmall);

        SetTimer(hWnd, IDT_REFRESH, 2000, NULL);
        break;
    }

    case WM_TIMER:
        if (wParam == IDT_REFRESH) RefreshProcessList(hWnd, true);
        break;

    // ── Background ──────────────────────────────────────────────────────────
    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rc; GetClientRect(hWnd, &rc);
        FillRect(hdc, &rc, g_brBG);
        return 1;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hWnd, &ps);
        RECT cr; GetClientRect(hWnd, &cr);

        // Double-buffer
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, cr.right, cr.bottom);
        SelectObject(mem, bmp);
        FillRect(mem, &cr, g_brBG);

        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);

        // Header strip (y 0-68)
        SolidBrush hdrBr(Color(255,16,16,28));
        g.FillRectangle(&hdrBr, 0, 0, cr.right, 68);

        // Logo in header (36×36 at x=13,y=16)
        if (g_logo && g_logo->GetLastStatus() == Ok) {
            g.DrawImage(g_logo, 13, 16, 36, 36);
        } else {
            DrawRoundRect(g, 13, 16, 36, 36, 6,
                Color(255,124,58,237), Color(255,167,139,250));
        }

        // App title
        HFONT old = (HFONT)SelectObject(mem, g_fntHead);
        SetTextColor(mem, C_TEXT); SetBkMode(mem, TRANSPARENT);
        RECT tr = {58, 17, cr.right-10, 40};
        DrawTextA(mem,"VetoAltManager",-1,&tr,DT_LEFT|DT_SINGLELINE);
        SelectObject(mem, g_fntSmall);
        SetTextColor(mem, C_DIM);
        RECT sr = {59, 41, cr.right-10, 58};
        DrawTextA(mem,"1.8.9 Alt Manager",-1,&sr,DT_LEFT|DT_SINGLELINE);
        SelectObject(mem, old);

        // Header bottom border
        Pen sepPen(Color(255,50,50,80), 1.f);
        g.DrawLine(&sepPen, 0, 68, cr.right, 68);

        // Separator above buttons
        g.DrawLine(&sepPen, 15, 370, cr.right-15, 370);

        // Separator above info
        g.DrawLine(&sepPen, 15, 416, cr.right-15, 416);

        // Info text
        SelectObject(mem, g_fntSmall);
        SetTextColor(mem, C_DIM); SetBkMode(mem, TRANSPARENT);
        RECT ir = {15, 422, cr.right-15, 465};
        DrawTextA(mem,
            "1. Launch Minecraft 1.8.9 (Vanilla / OptiFine / Lunar / Badlion)\r\n"
            "2. Process appears automatically — select it\r\n"
            "3. Enter username \xbb 'Change Alt'   |   'Restore Original' to go back to premium",
            -1, &ir, DT_LEFT|DT_WORDBREAK);

        BitBlt(hdc, 0,0,cr.right,cr.bottom, mem,0,0,SRCCOPY);
        DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hWnd, &ps);
        break;
    }

    // ── Control colors ──────────────────────────────────────────────────────
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        SetBkColor(hdc,   C_BG2);
        SetTextColor(hdc, C_TEXT);
        return (LRESULT)g_brBG2;
    }
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        HWND ctrl = (HWND)lParam;
        // Status bar gets slightly different dim color
        if (ctrl == g_hStatus) SetTextColor(hdc, C_DIM);
        else                   SetTextColor(hdc, C_TEXT);
        SetBkColor(hdc, C_BG);
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)g_brBG;
    }
    case WM_CTLCOLORLISTBOX: {
        HDC hdc = (HDC)wParam;
        SetBkColor(hdc,   C_BG2);
        SetTextColor(hdc, C_TEXT);
        return (LRESULT)g_brBG2;
    }

    // ── Owner-draw buttons ──────────────────────────────────────────────────
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* d = (DRAWITEMSTRUCT*)lParam;
        if (d->CtlType == ODT_BUTTON) {
            Graphics g(d->hDC);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            bool pressed = (d->itemState & ODS_SELECTED) != 0;

            // Background fill
            Color fill  = pressed ? Color(255,44,44,70) : Color(255,28,28,46);
            Color border= Color(255,124,58,237);

            // "Restore" gets a subtler border
            if (d->CtlID == IDC_RESTORE)
                border = Color(255,90,90,130);

            DrawRoundRect(g, d->rcItem.left+1, d->rcItem.top+1,
                d->rcItem.right - d->rcItem.left - 2,
                d->rcItem.bottom - d->rcItem.top - 2,
                5, fill, border);

            // Label
            char txt[64]{}; GetWindowTextA(d->hwndItem, txt, 64);
            HFONT old = (HFONT)SelectObject(d->hDC, g_fntNorm);
            SetTextColor(d->hDC, C_TEXT);
            SetBkMode(d->hDC, TRANSPARENT);
            DrawTextA(d->hDC, txt, -1, &d->rcItem,
                DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            SelectObject(d->hDC, old);
            return TRUE;
        }
        // Owner-draw listbox items
        if (d->CtlType == ODT_LISTBOX) {
            if (d->itemID == (UINT)-1) break;
            char txt[256]{}; SendMessageA(g_hList, LB_GETTEXT, d->itemID, (LPARAM)txt);

            bool sel = (d->itemState & ODS_SELECTED) != 0;
            COLORREF bg = sel ? C_BG3 : C_BG2;

            // Row background
            HBRUSH rowBr = CreateSolidBrush(bg);
            FillRect(d->hDC, &d->rcItem, rowBr);
            DeleteObject(rowBr);

            // Colored tag  "[Vanilla]" / "[Lunar Client]" / etc.
            const char* bracket = strchr(txt, ']');
            RECT rc = d->rcItem;
            SetBkMode(d->hDC, TRANSPARENT);

            int xOff = rc.left + 8;
            if (bracket) {
                char tag[32]{};
                int tagLen = (int)(bracket - txt + 1);
                if (tagLen < 32) { strncpy(tag, txt, tagLen); tag[tagLen]=0; }

                HFONT old2 = (HFONT)SelectObject(d->hDC, g_fntSmall);
                SetTextColor(d->hDC, sel ? C_TEXT : TagColor(tag));
                RECT tagRc = {xOff, rc.top, xOff+130, rc.bottom};
                DrawTextA(d->hDC, tag, -1, &tagRc, DT_LEFT|DT_VCENTER|DT_SINGLELINE);

                SIZE tsz{}; GetTextExtentPoint32A(d->hDC, tag, (int)strlen(tag), &tsz);
                xOff += tsz.cx + 8;

                // Rest of string
                SetTextColor(d->hDC, sel ? C_TEXT : C_DIM);
                RECT restRc = {xOff, rc.top, rc.right-4, rc.bottom};
                DrawTextA(d->hDC, bracket+1, -1, &restRc, DT_LEFT|DT_VCENTER|DT_SINGLELINE);
                SelectObject(d->hDC, old2);
            } else {
                HFONT old2 = (HFONT)SelectObject(d->hDC, g_fntSmall);
                SetTextColor(d->hDC, sel ? C_TEXT : C_DIM);
                RECT allRc = {xOff, rc.top, rc.right-4, rc.bottom};
                DrawTextA(d->hDC, txt, -1, &allRc, DT_LEFT|DT_VCENTER|DT_SINGLELINE);
                SelectObject(d->hDC, old2);
            }

            // Bottom rule
            HPEN rPen = CreatePen(PS_SOLID,1, C_BG3);
            HPEN old3 = (HPEN)SelectObject(d->hDC, rPen);
            MoveToEx(d->hDC, rc.left, rc.bottom-1, NULL);
            LineTo  (d->hDC, rc.right,rc.bottom-1);
            SelectObject(d->hDC, old3); DeleteObject(rPen);
            return TRUE;
        }
        break;
    }
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT* m = (MEASUREITEMSTRUCT*)lParam;
        if (m->CtlType == ODT_LISTBOX) m->itemHeight = 30;
        break;
    }

    // ── Commands ─────────────────────────────────────────────────────────────
    case WM_COMMAND: {
        int id = LOWORD(wParam), code = HIWORD(wParam);
        switch (id) {

        case IDC_INJECT: {
            int sel = (int)SendMessage(g_hList, LB_GETCURSEL, 0, 0);
            if (sel==LB_ERR||sel>=(int)g_pids.size())
                { SetWindowTextA(g_hStatus,"Select a process first."); break; }
            DWORD pid = g_pids[sel];
            char dllPath[MAX_PATH]; GetModuleFileNameA(NULL,dllPath,MAX_PATH);
            char* sl=strrchr(dllPath,'\\'); if(sl)*sl=0;
            strcat(dllPath,"\\SessionChanger.dll");
            SetWindowTextA(g_hStatus,"Injecting DLL...");
            EnableWindow(g_hBtnInject,FALSE);
            if (InjectDLL(pid,dllPath)) {
                char buf[128]; snprintf(buf,sizeof(buf),"DLL injected into PID %lu",pid);
                SetWindowTextA(g_hStatus,buf);
            }
            EnableWindow(g_hBtnInject,TRUE);
            break;
        }

        case IDC_CHANGE: {
            char username[64]{}; GetWindowTextA(g_hEdit,username,63);
            char* e=username+strlen(username)-1;
            while(e>username&&(*e==' '||*e=='\t'))*e--=0;
            char* s=username; while(*s==' '||*s=='\t')s++;
            if(!*s){SetWindowTextA(g_hStatus,"Enter a username first.");break;}

            char cmd[256]; snprintf(cmd,sizeof(cmd),"CHANGE %s",s);
            char resp[256]{};
            EnableWindow(g_hBtnChange,FALSE);
            SetWindowTextA(g_hStatus,"Sending alt change...");

            if (!SendPipeCommand(cmd,resp,sizeof(resp))) {
                SetWindowTextA(g_hStatus,"Auto-injecting DLL...");
                int sel=(int)SendMessage(g_hList,LB_GETCURSEL,0,0);
                if(sel==LB_ERR||sel>=(int)g_pids.size())
                    {SetWindowTextA(g_hStatus,"Select a process first.");EnableWindow(g_hBtnChange,TRUE);break;}
                DWORD pid=g_pids[sel];
                char dllPath[MAX_PATH]; GetModuleFileNameA(NULL,dllPath,MAX_PATH);
                char* sl=strrchr(dllPath,'\\'); if(sl)*sl=0; strcat(dllPath,"\\SessionChanger.dll");
                if(GetFileAttributesA(dllPath)==INVALID_FILE_ATTRIBUTES)
                    {char b[MAX_PATH+64];snprintf(b,sizeof(b),"DLL not found: %s",dllPath);
                     SetWindowTextA(g_hStatus,b);EnableWindow(g_hBtnChange,TRUE);break;}
                bool ok=false;
                for(int r=0;r<3&&!ok;r++){if(r)Sleep(500);ok=InjectDLL(pid,dllPath);}
                if(!ok){EnableWindow(g_hBtnChange,TRUE);break;}
                SetWindowTextA(g_hStatus,"Waiting for DLL pipe...");
                bool ready=false;
                for(int i=0;i<60;i++){char pong[32]{};
                    if(SendPipeCommand("PING",pong,32)&&strcmp(pong,"PONG")==0){ready=true;break;}
                    Sleep(500);}
                if(!ready){SetWindowTextA(g_hStatus,"Pipe not responding. Try again.");
                    EnableWindow(g_hBtnChange,TRUE);break;}
                SendPipeCommand(cmd,resp,sizeof(resp));
            }
            char buf[256]; snprintf(buf,sizeof(buf),"Alt change: %s",resp[0]?resp:"no response");
            SetWindowTextA(g_hStatus,buf);
            EnableWindow(g_hBtnChange,TRUE);
            break;
        }

        case IDC_RESTORE: {
            char resp[256]{};
            EnableWindow(g_hBtnRestore,FALSE);
            SetWindowTextA(g_hStatus,"Restoring original session...");
            if (!SendPipeCommand("RESTORE",resp,sizeof(resp)))
                SetWindowTextA(g_hStatus,"Pipe not ready — inject DLL first.");
            else {
                char buf[256];
                snprintf(buf,sizeof(buf),"Restore: %s",
                    strcmp(resp,"OK")==0?"session restored to original account":resp);
                SetWindowTextA(g_hStatus,buf);
            }
            EnableWindow(g_hBtnRestore,TRUE);
            break;
        }

        case IDC_LIST:
            if (code==LBN_DBLCLK) SendMessage(hWnd,WM_COMMAND,IDC_INJECT,0);
            break;
        }
        break;
    }

    case WM_CLOSE:   DestroyWindow(hWnd); break;
    case WM_DESTROY: KillTimer(hWnd,IDT_REFRESH); PostQuitMessage(0); break;
    default: return DefWindowProcA(hWnd,msg,wParam,lParam);
    }
    return 0;
}

// ════════════════════════════════════════════════════════════════════════════
// ENTRY POINT
// ════════════════════════════════════════════════════════════════════════════
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nShow) {
    g_hInst = hInst;

    // GDI+
    GdiplusStartupInput si; GdiplusStartup(&g_gdip, &si, NULL);

    // Load logo
    std::string lpath = GetLogoPath();
    wchar_t wlp[MAX_PATH]{}; MultiByteToWideChar(CP_ACP,0,lpath.c_str(),-1,wlp,MAX_PATH);
    g_logo = new Image(wlp);
    if (g_logo->GetLastStatus() != Ok) { delete g_logo; g_logo = NULL; }

    // Brushes
    g_brBG  = CreateSolidBrush(C_BG);
    g_brBG2 = CreateSolidBrush(C_BG2);
    g_brBG3 = CreateSolidBrush(C_BG3);

    // Fonts
    g_fntTitle = MakeFont(20, true);
    g_fntSub   = MakeFont(10, false);
    g_fntHead  = MakeFont(14, true);
    g_fntNorm  = MakeFont(11, false);
    g_fntSmall = MakeFont( 9, false);

    INITCOMMONCONTROLSEX icex={sizeof(icex),ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icex);

    // ── Register main window class ──
    {
        WNDCLASSA wc{};
        wc.lpfnWndProc   = WndProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursor(NULL,IDC_ARROW);
        wc.hbrBackground = g_brBG;
        wc.lpszClassName = "VetoMain";
        RegisterClassA(&wc);
    }

    // ── Register splash class ──
    {
        WNDCLASSA wc{};
        wc.lpfnWndProc   = SplashProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursor(NULL,IDC_ARROW);
        wc.hbrBackground = CreateSolidBrush(C_BG);
        wc.style         = CS_DROPSHADOW;
        wc.lpszClassName = "VetoSplash";
        RegisterClassA(&wc);
    }

    // ── Create main window (hidden) ──
    g_hMain = CreateWindowExA(0,"VetoMain","VetoAltManager",
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
        CW_USEDEFAULT,CW_USEDEFAULT, 522,476, NULL,NULL,hInst,NULL);

    // ── Create & center splash ──
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int splW=450, splH=295;
    g_hSplash = CreateWindowExA(
        WS_EX_TOPMOST|WS_EX_TOOLWINDOW,
        "VetoSplash","",
        WS_POPUP|WS_VISIBLE,
        (sw-splW)/2,(sh-splH)/2, splW,splH,
        NULL,NULL,hInst,NULL);
    ApplyDarkFrame(g_hSplash);

    MSG msg;
    while (GetMessageA(&msg,NULL,0,0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    // Cleanup
    if (g_logo) { delete g_logo; g_logo=NULL; }
    GdiplusShutdown(g_gdip);
    DeleteObject(g_brBG); DeleteObject(g_brBG2); DeleteObject(g_brBG3);
    DeleteObject(g_fntTitle); DeleteObject(g_fntSub); DeleteObject(g_fntHead);
    DeleteObject(g_fntNorm); DeleteObject(g_fntSmall);
    return (int)msg.wParam;
}

// ════════════════════════════════════════════════════════════════════════════
// PROCESS LIST
// ════════════════════════════════════════════════════════════════════════════
void RefreshProcessList(HWND hWnd, bool silent) {
    DWORD selPid = 0;
    int sel = (int)SendMessage(g_hList, LB_GETCURSEL, 0, 0);
    if (sel!=LB_ERR && sel<(int)g_pids.size()) selPid = g_pids[sel];

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if (snap==INVALID_HANDLE_VALUE) return;

    std::vector<std::pair<DWORD,std::string>> found;
    PROCESSENTRY32 pe{sizeof(pe)};
    if (Process32First(snap,&pe)) do {
        char exe[MAX_PATH]; strncpy(exe,pe.szExeFile,MAX_PATH-1); _strlwr(exe);
        if (!strstr(exe,"java")) continue;
        std::string lbl = GetClientLabel(pe.th32ProcessID);
        if (lbl.empty()) continue;
        std::string disp = lbl+"  "+pe.szExeFile+"  (PID "+std::to_string(pe.th32ProcessID)+")";
        found.push_back({pe.th32ProcessID, disp});
    } while(Process32Next(snap,&pe));
    CloseHandle(snap);

    bool changed = found.size()!=g_pids.size();
    if (!changed) for (size_t i=0;i<found.size();i++)
        if (found[i].first!=g_pids[i]||found[i].second!=g_names[i]){changed=true;break;}

    if (changed) {
        SendMessage(g_hList,LB_RESETCONTENT,0,0);
        g_pids.clear(); g_names.clear();
        int restoreSel=0;
        for (auto& p:found) {
            int idx=(int)SendMessage(g_hList,LB_ADDSTRING,0,(LPARAM)p.second.c_str());
            SendMessage(g_hList,LB_SETITEMDATA,idx,p.first);
            if(p.first==selPid) restoreSel=(int)g_pids.size();
            g_pids.push_back(p.first); g_names.push_back(p.second);
        }
        if (!g_pids.empty()) SendMessage(g_hList,LB_SETCURSEL,restoreSel,0);
    }

    if (!silent && g_hStatus) {
        char buf[128];
        snprintf(buf,sizeof(buf), found.empty()
            ? "No Minecraft processes found. Launch Minecraft first."
            : "Found %zu Minecraft process(es).", found.size());
        SetWindowTextA(g_hStatus,buf);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// DLL INJECTION
// ════════════════════════════════════════════════════════════════════════════
bool InjectDLL(DWORD pid, const char* dllPath) {
    HANDLE hp = OpenProcess(PROCESS_CREATE_THREAD|PROCESS_QUERY_INFORMATION|
        PROCESS_VM_OPERATION|PROCESS_VM_WRITE|PROCESS_VM_READ, FALSE, pid);
    if (!hp) {
        char b[256]; snprintf(b,sizeof(b),"OpenProcess failed (PID %lu, err=%lu). Run as Admin.",pid,GetLastError());
        SetWindowTextA(g_hStatus,b); return false;
    }
    size_t sz=strlen(dllPath)+1;
    LPVOID mem=VirtualAllocEx(hp,NULL,sz,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if (!mem){char b[128];snprintf(b,128,"VirtualAllocEx failed (%lu)",GetLastError());
        SetWindowTextA(g_hStatus,b);CloseHandle(hp);return false;}
    if (!WriteProcessMemory(hp,mem,dllPath,sz,NULL)){char b[128];snprintf(b,128,"WriteProcessMemory failed (%lu)",GetLastError());
        SetWindowTextA(g_hStatus,b);VirtualFreeEx(hp,mem,0,MEM_RELEASE);CloseHandle(hp);return false;}
    LPVOID ll=(LPVOID)GetProcAddress(GetModuleHandleA("kernel32.dll"),"LoadLibraryA");
    HANDLE ht=CreateRemoteThread(hp,NULL,0,(LPTHREAD_START_ROUTINE)ll,mem,0,NULL);
    if (!ht){DWORD e=GetLastError();char b[200];snprintf(b,200,"CreateRemoteThread failed (%lu). %s",e,
        e==5?"Access denied — run as Admin.":e==8?"Not enough memory.":"Try as Administrator.");
        SetWindowTextA(g_hStatus,b);VirtualFreeEx(hp,mem,0,MEM_RELEASE);CloseHandle(hp);return false;}
    WaitForSingleObject(ht,10000);
    DWORD ec=0; GetExitCodeThread(ht,&ec);
    if (!ec){SetWindowTextA(g_hStatus,"LoadLibraryA returned NULL — DLL missing or bad imports.");
        VirtualFreeEx(hp,mem,0,MEM_RELEASE);CloseHandle(ht);CloseHandle(hp);return false;}
    VirtualFreeEx(hp,mem,0,MEM_RELEASE);CloseHandle(ht);CloseHandle(hp);
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// PIPE
// ════════════════════════════════════════════════════════════════════════════
bool SendPipeCommand(const char* cmd, char* resp, int sz) {
    HANDLE h=CreateFileA(PIPE_NAME,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,0,NULL);
    if (h==INVALID_HANDLE_VALUE) return false;
    DWORD w=0;
    if (!WriteFile(h,cmd,(DWORD)strlen(cmd),&w,NULL)){CloseHandle(h);return false;}
    FlushFileBuffers(h);
    DWORD r=0;
    if (!ReadFile(h,resp,sz-1,&r,NULL)){CloseHandle(h);return false;}
    resp[r]=0; CloseHandle(h); return true;
}
