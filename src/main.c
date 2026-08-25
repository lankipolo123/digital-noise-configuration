/* TX Lite - single-page Win32 UI over connection.h/device.h.
 * Consolidates sdr_controller's Dashboard + Device Control pages (minus the
 * Communication page's terminal log / activity chart) into one window.
 * No Qt, no pywebview, no vendor DLL - just user32/gdi32/kernel32/advapi32.
 */
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "resource.h"
#include "connection.h"
#include "device.h"

#define CLIENT_WIDTH  700
#define CLIENT_HEIGHT 378

static const int BAUD_OPTIONS[] = { 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, 2000000 };
#define BAUD_OPTIONS_COUNT 9
#define BAUD_DEFAULT_INDEX 4 /* 115200 */

static const int DATABITS_OPTIONS[] = { 5, 6, 7, 8 };
#define DATABITS_OPTIONS_COUNT 4
#define DATABITS_DEFAULT_INDEX 3 /* 8 */

static const char *const PARITY_LABELS[] = { "None", "Odd", "Even", "Mark", "Space" };
static const char PARITY_CODES[] = { 'N', 'O', 'E', 'M', 'S' };
#define PARITY_OPTIONS_COUNT 5

static const int BANDWIDTH_OPTIONS[] = { 10, 20, 50, 100, 150, 200, 250, 300 };
#define BANDWIDTH_OPTIONS_COUNT 8
#define BANDWIDTH_DEFAULT_INDEX 3 /* 100 MHz */
#define BANDWIDTH_UNCONFIRMED_MHZ 300

static const int POWER_OPTIONS[] = { 0, -6, -12 };
#define POWER_OPTIONS_COUNT 3

static const int STEP_OPTIONS[] = { 1, 10, 50, 100 };
#define STEP_OPTIONS_COUNT 4
#define STEP_DEFAULT_INDEX 1 /* 10 MHz */

#define DEFAULT_FREQUENCY_MHZ 2450

/* MILITRONIX Dark palette: the real brand colors (charcoal + the logo's
 * blue) in dark shades - dark charcoal page/panels, light text, the same
 * blue family on buttons, a brighter blue for panel titles so they pop
 * against the dark fill. Flat fills only, no gradients, no theming. */
#define COLOR_APP_PAGE_BG   RGB(32, 33, 36)
#define COLOR_APP_PANEL_BG  RGB(43, 45, 49)
#define COLOR_APP_TEXT      RGB(232, 233, 234)
#define COLOR_APP_MUTED     RGB(154, 156, 160)
#define COLOR_APP_ACCENT    RGB(26, 133, 184)
#define COLOR_APP_ACCENT_DIS RGB(58, 74, 82)
#define COLOR_APP_HEADER    RGB(58, 168, 221)
#define COLOR_APP_FIELD_BG  RGB(23, 24, 26)
#define COLOR_APP_CONNECTED RGB(58, 181, 94)
#define COLOR_APP_DISCONNECTED RGB(224, 90, 90)
#define COLOR_APP_DOT       RGB(50, 52, 57)
#define COLOR_APP_PANEL_BORDER RGB(63, 66, 71)
#define COLOR_APP_SILVER    RGB(176, 180, 186)

/* Chamfered-corner panels: how much to cut off each corner. */
#define PANEL_CHAMFER 10

/* Background dot grid: drawn once across the whole client rect in
 * WM_ERASEBKGND, before any panel paints on top of it - panels are opaque
 * across their full rect, so the dots end up visible only in the gaps
 * around the sections, never inside one. */
#define DOT_GRID_SPACING 8
#define DOT_GRID_SIZE 2

static HINSTANCE g_hinst;
static HWND g_hwnd;
static HFONT g_font;
static HFONT g_mono_font;
static HFONT g_header_font;
static WNDPROC g_panel_orig_proc;
static HBRUSH g_brush_warn;
static HBRUSH g_brush_panel;
static HBRUSH g_brush_dot;
static HBRUSH g_brush_page;
static HBRUSH g_brush_field;
static HBRUSH g_brush_accent;
static HBRUSH g_brush_accent_dis;
static HBRUSH g_brush_silver;
static HBRUSH g_brush_light;

/* Custom Proceed/Cancel confirm popup (see show_confirm_dialog) - state
 * for the one dialog that can be open at a time. */
static bool g_confirm_class_registered;
static int g_confirm_result;
static const char *g_confirm_message;

static Connection g_conn;
static Device g_device;

static void ui_refresh_status(void);
static void ui_show_warning(const char *message);
static void ui_clear_warning(void);
static void ui_update_connect_button(bool connected);
static void refresh_port_list(void);

/* ---- small control-creation helper ---- */

static HWND add_ctrl(HWND parent, LPCSTR cls, LPCSTR text, DWORD style, int x, int y, int w, int h, int id) {
    HWND ctrl = CreateWindowExA(0, cls, text, style | WS_CHILD | WS_VISIBLE,
                                 x, y, w, h, parent, (HMENU)(INT_PTR)id, g_hinst, NULL);
    if (ctrl) {
        SendMessageA(ctrl, WM_SETFONT, (WPARAM)g_font, (LPARAM)TRUE);
    }
    return ctrl;
}

/* Chamfered-corner panel painting: replaces the STATIC control's default
 * WM_PAINT/WM_ERASEBKGND entirely, so the panel fills and outlines itself
 * as an octagon (rectangle with corners cut at 45 degrees) instead of a
 * plain square-cornered box. All add_panel() instances are the same
 * "STATIC" system class, so the pre-subclass window proc is identical
 * across them - captured once and reused. */
static LRESULT CALLBACK panel_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ERASEBKGND) {
        return 1;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc;
        RECT rc;
        POINT pts[8];
        POINT tri[3];
        HBRUSH old_brush;
        HPEN pen, old_pen, silver_pen, old_silver_pen;
        int c = PANEL_CHAMFER;

        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);

        pts[0].x = rc.left;              pts[0].y = rc.top + c;
        pts[1].x = rc.left + c;          pts[1].y = rc.top;
        pts[2].x = rc.right - 1 - c;     pts[2].y = rc.top;
        pts[3].x = rc.right - 1;         pts[3].y = rc.top + c;
        pts[4].x = rc.right - 1;         pts[4].y = rc.bottom - 1 - c;
        pts[5].x = rc.right - 1 - c;     pts[5].y = rc.bottom - 1;
        pts[6].x = rc.left + c;          pts[6].y = rc.bottom - 1;
        pts[7].x = rc.left;              pts[7].y = rc.bottom - 1 - c;

        old_brush = (HBRUSH)SelectObject(hdc, g_brush_panel);
        pen = CreatePen(PS_SOLID, 1, COLOR_APP_PANEL_BORDER);
        old_pen = (HPEN)SelectObject(hdc, pen);

        Polygon(hdc, pts, 8);

        SelectObject(hdc, old_pen);
        DeleteObject(pen);

        /* A small silver triangle floating near each corner, inset from
         * the panel's chamfer edge rather than flush against it - leaves
         * a visible gap of bare page background between the accent and
         * the border instead of the triangle sticking directly to it. */
        {
            int t = c - 4; /* smaller than the chamfer cut itself, so it
                             * doesn't reach the chamfer's diagonal edge */

            silver_pen = CreatePen(PS_SOLID, 1, COLOR_APP_SILVER);
            old_silver_pen = (HPEN)SelectObject(hdc, silver_pen);
            SelectObject(hdc, g_brush_silver);

            tri[0].x = rc.left;       tri[0].y = rc.top;
            tri[1].x = rc.left + t;   tri[1].y = rc.top;
            tri[2].x = rc.left;       tri[2].y = rc.top + t;
            Polygon(hdc, tri, 3);

            tri[0].x = rc.right - 1;      tri[0].y = rc.top;
            tri[1].x = rc.right - 1 - t;  tri[1].y = rc.top;
            tri[2].x = rc.right - 1;      tri[2].y = rc.top + t;
            Polygon(hdc, tri, 3);

            tri[0].x = rc.right - 1;      tri[0].y = rc.bottom - 1;
            tri[1].x = rc.right - 1 - t;  tri[1].y = rc.bottom - 1;
            tri[2].x = rc.right - 1;      tri[2].y = rc.bottom - 1 - t;
            Polygon(hdc, tri, 3);

            tri[0].x = rc.left;       tri[0].y = rc.bottom - 1;
            tri[1].x = rc.left + t;   tri[1].y = rc.bottom - 1;
            tri[2].x = rc.left;       tri[2].y = rc.bottom - 1 - t;
            Polygon(hdc, tri, 3);
        }

        SelectObject(hdc, old_silver_pen);
        DeleteObject(silver_pen);
        SelectObject(hdc, old_brush);

        EndPaint(hwnd, &ps);
        return 0;
    }
    return CallWindowProcA(g_panel_orig_proc, hwnd, msg, wParam, lParam);
}

/* A section "panel": a chamfered-corner rectangle with no built-in
 * caption - the text title is a separate add_header() label placed
 * inside it, so the design isn't tied to the classic BS_GROUPBOX
 * notched-border look. */
static HWND add_panel(HWND parent, int x, int y, int w, int h) {
    HWND ctrl = add_ctrl(parent, "STATIC", NULL, SS_LEFT, x, y, w, h, 0);
    if (ctrl) {
        if (!g_panel_orig_proc) {
            g_panel_orig_proc = (WNDPROC)GetWindowLongPtrA(ctrl, GWLP_WNDPROC);
        }
        SetWindowLongPtrA(ctrl, GWLP_WNDPROC, (LONG_PTR)panel_subclass_proc);
    }
    return ctrl;
}

/* Section title text, drawn inside the panel using the bold header font;
 * WM_CTLCOLORSTATIC recognizes that font and colors it with the accent. */
static HWND add_header(HWND parent, LPCSTR text, int x, int y, int w, int h) {
    HWND ctrl = add_ctrl(parent, "STATIC", text, SS_LEFT, x, y, w, h, 0);
    if (ctrl && g_header_font) {
        SendMessageA(ctrl, WM_SETFONT, (WPARAM)g_header_font, (LPARAM)TRUE);
    }
    return ctrl;
}

static void draw_dot_grid(HDC hdc, const RECT *rc) {
    int x, y;
    for (y = DOT_GRID_SPACING / 2; y < rc->bottom; y += DOT_GRID_SPACING) {
        for (x = DOT_GRID_SPACING / 2; x < rc->right; x += DOT_GRID_SPACING) {
            RECT dot;
            dot.left = x;
            dot.top = y;
            dot.right = x + DOT_GRID_SIZE;
            dot.bottom = y + DOT_GRID_SIZE;
            FillRect(hdc, &dot, g_brush_dot);
        }
    }
}

/* ---- Proceed/Cancel confirm popup (replaces MessageBoxA's Yes/No for
 * dialogs that need specific button wording) ---- */

static LRESULT CALLBACK confirm_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            add_ctrl(hwnd, "STATIC", g_confirm_message, SS_LEFT | SS_NOPREFIX, 16, 16, 328, 50, 0);
            add_ctrl(hwnd, "BUTTON", "Proceed", BS_OWNERDRAW | WS_TABSTOP, 95, 76, 90, 28, IDOK);
            add_ctrl(hwnd, "BUTTON", "Cancel", BS_OWNERDRAW | WS_TABSTOP, 195, 76, 90, 28, IDCANCEL);
            return 0;

        case WM_DRAWITEM: {
            DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lParam;
            if (dis->CtlType == ODT_BUTTON) {
                char text[32];
                RECT rc = dis->rcItem;
                GetWindowTextA(dis->hwndItem, text, sizeof(text));
                if (dis->CtlID == IDOK) {
                    FillRect(dis->hDC, &rc, g_brush_accent);
                    SetTextColor(dis->hDC, RGB(255, 255, 255));
                } else {
                    HPEN pen = CreatePen(PS_SOLID, 1, COLOR_APP_PANEL_BORDER);
                    HPEN old_pen = (HPEN)SelectObject(dis->hDC, pen);
                    HBRUSH old_brush = (HBRUSH)SelectObject(dis->hDC, g_brush_panel);
                    Rectangle(dis->hDC, rc.left, rc.top, rc.right, rc.bottom);
                    SelectObject(dis->hDC, old_brush);
                    SelectObject(dis->hDC, old_pen);
                    DeleteObject(pen);
                    SetTextColor(dis->hDC, COLOR_APP_TEXT);
                }
                SetBkMode(dis->hDC, TRANSPARENT);
                DrawTextA(dis->hDC, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                if (dis->itemState & ODS_FOCUS) {
                    RECT focus_rc = rc;
                    InflateRect(&focus_rc, -3, -3);
                    DrawFocusRect(dis->hDC, &focus_rc);
                }
                return TRUE;
            }
            break;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, COLOR_APP_TEXT);
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)g_brush_page;
        }

        case WM_COMMAND:
            if (HIWORD(wParam) == BN_CLICKED) {
                if (LOWORD(wParam) == IDOK) {
                    g_confirm_result = 1;
                    DestroyWindow(hwnd);
                } else if (LOWORD(wParam) == IDCANCEL) {
                    g_confirm_result = 0;
                    DestroyWindow(hwnd);
                }
            }
            return 0;

        case WM_CLOSE:
            g_confirm_result = 0;
            DestroyWindow(hwnd);
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* Modal Proceed/Cancel popup with app-styled buttons, for confirmations
 * that need specific wording MessageBoxA's fixed button sets can't give
 * (Yes/No, OK/Cancel, ...). Blocks (via its own message loop) until
 * answered; returns true only if Proceed was clicked. */
static bool show_confirm_dialog(HWND parent, const char *message) {
    RECT prc, wrc;
    HWND popup;
    MSG msg;
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    DWORD ex_style = WS_EX_DLGMODALFRAME;
    int x, y;

    if (!g_confirm_class_registered) {
        WNDCLASSEXA wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = confirm_wnd_proc;
        wc.hInstance = g_hinst;
        wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
        wc.hbrBackground = g_brush_page;
        wc.lpszClassName = "TxLiteConfirmDialog";
        RegisterClassExA(&wc);
        g_confirm_class_registered = true;
    }

    /* wrc starts as the desired CLIENT rect (360x160, matching the child
     * control layout in WM_CREATE) and grows to the required window rect
     * so the title bar/border don't eat into that client area. */
    wrc.left = 0;
    wrc.top = 0;
    wrc.right = 360;
    wrc.bottom = 130;
    AdjustWindowRectEx(&wrc, style, FALSE, ex_style);

    GetWindowRect(parent, &prc);
    x = prc.left + ((prc.right - prc.left) - (wrc.right - wrc.left)) / 2;
    y = prc.top + ((prc.bottom - prc.top) - (wrc.bottom - wrc.top)) / 2;

    g_confirm_result = 0;
    g_confirm_message = message;

    EnableWindow(parent, FALSE);
    popup = CreateWindowExA(ex_style, "TxLiteConfirmDialog", "Confirm", style,
                             x, y, wrc.right - wrc.left, wrc.bottom - wrc.top,
                             parent, NULL, g_hinst, NULL);
    if (popup) {
        ShowWindow(popup, SW_SHOW);
        while (IsWindow(popup) && GetMessageA(&msg, NULL, 0, 0)) {
            if (!IsDialogMessageA(popup, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
        }
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    return g_confirm_result == 1;
}

/* ---- device/connection -> UI callbacks (single global window, so these
 * just reach into the globals above rather than threading ctx through) ---- */

static void conn_on_connected_changed(bool connected, void *ctx) {
    (void)ctx;
    device_on_connected_changed(connected, &g_device);
    ui_update_connect_button(connected);
}

static void conn_on_frame(const ProtoParsedFrame *frame, void *ctx) {
    (void)ctx;
    ui_clear_warning();
    device_on_frame(frame, &g_device);
}

static void conn_on_raw_tx(const uint8_t *data, uint8_t len, void *ctx) {
    char buf[128];
    int pos = 0, i;
    (void)ctx;
    for (i = 0; i < len && pos < (int)sizeof(buf) - 6; i++) {
        pos += wsprintfA(buf + pos, "%02X | ", data[i]);
    }
    buf[pos] = '\0';
    SetDlgItemTextA(g_hwnd, IDC_TX_EDIT, buf);
}

static void conn_on_raw_rx(const uint8_t *data, uint16_t len, void *ctx) {
    char buf[340];
    int pos = 0, i;
    int n = (len > 64) ? 64 : (int)len; /* real frames are tiny; this just bounds a garbage burst */
    (void)ctx;
    for (i = 0; i < n && pos < (int)sizeof(buf) - 6; i++) {
        pos += wsprintfA(buf + pos, "%02X | ", data[i]);
    }
    buf[pos] = '\0';
    SetDlgItemTextA(g_hwnd, IDC_RX_EDIT, buf);
}

static void conn_on_error(const char *message, void *ctx) {
    (void)ctx;
    ui_show_warning(message);
}

static void dev_on_state_changed(void *ctx) {
    (void)ctx;
    ui_refresh_status();
}

static void dev_on_command_timeout(const char *message, void *ctx) {
    (void)ctx;
    ui_show_warning(message);
}

static void dev_on_command_failed(const char *message, void *ctx) {
    (void)ctx;
    ui_show_warning(message);
}

/* ---- UI update helpers ---- */

static void ui_refresh_status(void) {
    DeviceState *s = &g_device.state;
    char buf[64];

    SetDlgItemTextA(g_hwnd, IDC_CONN_STATUS_LBL, s->connected ? "Connected" : "Disconnected");

    if (s->frequency_mhz != DEVICE_UNKNOWN) {
        wsprintfA(buf, "%d MHz", s->frequency_mhz);
        SetDlgItemTextA(g_hwnd, IDC_OUTPUT_FREQ_LBL, buf);
        /* Don't clobber the frequency edit while the user is mid-edit,
         * same guard as the address box below. */
        if (GetFocus() != GetDlgItem(g_hwnd, IDC_FREQ_EDIT)) {
            SetDlgItemInt(g_hwnd, IDC_FREQ_EDIT, (UINT)s->frequency_mhz, FALSE);
        }
    } else {
        SetDlgItemTextA(g_hwnd, IDC_OUTPUT_FREQ_LBL, "-");
    }

    SetDlgItemTextA(g_hwnd, IDC_OUTPUT_PILL, s->output_on ? "ON" : "OFF");
    CheckDlgButton(g_hwnd, IDC_OUTPUT_CHECK, s->output_on ? BST_CHECKED : BST_UNCHECKED);

    /* Don't clobber the address box while the user is mid-edit, matching
     * the reference's hasFocus() guard. */
    if (GetFocus() != GetDlgItem(g_hwnd, IDC_ADDR_EDIT)) {
        SetDlgItemInt(g_hwnd, IDC_ADDR_EDIT, s->address, FALSE);
    }

    InvalidateRect(GetDlgItem(g_hwnd, IDC_CONN_STATUS_LBL), NULL, TRUE);
    InvalidateRect(GetDlgItem(g_hwnd, IDC_OUTPUT_PILL), NULL, TRUE);
}

static void ui_show_warning(const char *message) {
    char buf[300];
    wsprintfA(buf, "! %s", message);
    SetDlgItemTextA(g_hwnd, IDC_WARNING_LBL, buf);
    ShowWindow(GetDlgItem(g_hwnd, IDC_WARNING_LBL), SW_SHOW);
}

static void ui_clear_warning(void) {
    ShowWindow(GetDlgItem(g_hwnd, IDC_WARNING_LBL), SW_HIDE);
}

static void ui_update_connect_button(bool connected) {
    SetDlgItemTextA(g_hwnd, IDC_CONNECT_BTN, connected ? "Disconnect" : "Connect");
}

static void refresh_port_list(void) {
    char names[16][16];
    int count, i;
    char current[16];
    HWND combo = GetDlgItem(g_hwnd, IDC_PORT_COMBO);

    GetDlgItemTextA(g_hwnd, IDC_PORT_COMBO, current, sizeof(current));

    SendMessageA(combo, CB_RESETCONTENT, 0, 0);
    count = conn_list_ports(names, 16);
    if (count > 16) {
        count = 16;
    }
    for (i = 0; i < count; i++) {
        SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)names[i]);
    }
    if (current[0] != '\0') {
        SendMessageA(combo, CB_SELECTSTRING, (WPARAM)-1, (LPARAM)current);
    }
}

/* ---- command handlers ---- */

static void on_connect_clicked(void) {
    char port[16];
    int baud_idx, baud, databits_idx, databits, parity_idx;
    char parity;

    if (conn_is_connected(&g_conn)) {
        conn_disconnect(&g_conn);
        return;
    }

    GetDlgItemTextA(g_hwnd, IDC_PORT_COMBO, port, sizeof(port));
    if (port[0] == '\0') {
        return;
    }

    baud_idx = (int)SendDlgItemMessageA(g_hwnd, IDC_BAUD_COMBO, CB_GETCURSEL, 0, 0);
    baud = (int)SendDlgItemMessageA(g_hwnd, IDC_BAUD_COMBO, CB_GETITEMDATA, (WPARAM)baud_idx, 0);

    databits_idx = (int)SendDlgItemMessageA(g_hwnd, IDC_DATABITS_COMBO, CB_GETCURSEL, 0, 0);
    databits = (int)SendDlgItemMessageA(g_hwnd, IDC_DATABITS_COMBO, CB_GETITEMDATA, (WPARAM)databits_idx, 0);

    parity_idx = (int)SendDlgItemMessageA(g_hwnd, IDC_PARITY_COMBO, CB_GETCURSEL, 0, 0);
    if (parity_idx < 0 || parity_idx >= PARITY_OPTIONS_COUNT) {
        parity_idx = 0;
    }
    parity = PARITY_CODES[parity_idx];

    conn_connect(&g_conn, port, (DWORD)baud, parity, (uint8_t)databits);
}

static void step_frequency(int direction) {
    int step_idx, step_mhz, value;

    step_idx = (int)SendDlgItemMessageA(g_hwnd, IDC_STEP_COMBO, CB_GETCURSEL, 0, 0);
    step_mhz = (int)SendDlgItemMessageA(g_hwnd, IDC_STEP_COMBO, CB_GETITEMDATA, (WPARAM)step_idx, 0);

    value = (int)GetDlgItemInt(g_hwnd, IDC_FREQ_EDIT, NULL, FALSE);
    value += direction * step_mhz;
    if (value < PROTO_FREQ_MIN_MHZ) {
        value = PROTO_FREQ_MIN_MHZ;
    }
    if (value > PROTO_FREQ_MAX_MHZ) {
        value = PROTO_FREQ_MAX_MHZ;
    }
    SetDlgItemInt(g_hwnd, IDC_FREQ_EDIT, (UINT)value, FALSE);
}

static void on_apply_clicked(void) {
    uint8_t mode;
    int freq, bw_idx, bw_mhz, power_idx, power_db;
    ProtoStatus status;
    bool bw_unconfirmed;

    if (IsDlgButtonChecked(g_hwnd, IDC_RB_WHITE) == BST_CHECKED) {
        mode = PROTO_MODE_WHITE_NOISE;
    } else if (IsDlgButtonChecked(g_hwnd, IDC_RB_SWEEP) == BST_CHECKED) {
        mode = PROTO_MODE_LINEAR_SWEEP;
    } else if (IsDlgButtonChecked(g_hwnd, IDC_RB_COMB) == BST_CHECKED) {
        mode = PROTO_MODE_COMB_SPECTRUM;
    } else {
        mode = PROTO_MODE_SINGLE;
    }

    freq = (int)GetDlgItemInt(g_hwnd, IDC_FREQ_EDIT, NULL, FALSE);

    bw_idx = (int)SendDlgItemMessageA(g_hwnd, IDC_BW_COMBO, CB_GETCURSEL, 0, 0);
    bw_mhz = (int)SendDlgItemMessageA(g_hwnd, IDC_BW_COMBO, CB_GETITEMDATA, (WPARAM)bw_idx, 0);

    power_idx = (int)SendDlgItemMessageA(g_hwnd, IDC_POWER_COMBO, CB_GETCURSEL, 0, 0);
    power_db = (int)SendDlgItemMessageA(g_hwnd, IDC_POWER_COMBO, CB_GETITEMDATA, (WPARAM)power_idx, 0);

    bw_unconfirmed = (bw_mhz == BANDWIDTH_UNCONFIRMED_MHZ);

    if (bw_unconfirmed) {
        char msg[256];
        wsprintfA(msg,
                  "The selected bandwidth uses a guessed protocol byte value that hasn't been "
                  "verified against real hardware. Send anyway?");
        if (MessageBoxA(g_hwnd, msg, "Unconfirmed value", MB_YESNO | MB_ICONWARNING) != IDYES) {
            return;
        }
    }

    status = device_apply_signal_settings(&g_device, mode, (uint16_t)freq, (uint16_t)bw_mhz, power_db);
    if (status != PROTO_OK) {
        MessageBoxA(g_hwnd, "Invalid settings", "Invalid settings", MB_OK | MB_ICONWARNING);
    }
}

/* ---- layout ---- */

static void build_controls(HWND hwnd) {
    unsigned i;

    /* Two columns: left = connection/address/output, right = signal settings.
     * Left column is three stacked boxes; right column is one taller box,
     * so the next full-width row starts below whichever column is taller. */

    /* Every section is a plain bordered panel (add_panel) with its title as
     * a separate accent-colored label inside (add_header), rather than the
     * classic BS_GROUPBOX notched-caption look. Content starts 30px below
     * the panel top throughout (8px to the header, 16px of header, 6px
     * gap), so every section follows the same rhythm. */

    /* --- Left column (x=10, w=335): Connection & Settings, and Address &
     * Output as their own separate panels (never merged - each is its own
     * conceptual section). Row pitch is tightened to 24px throughout so
     * there's no wasted space inside either panel. --- */
    add_panel(hwnd, 10, 6, 335, 138);
    add_header(hwnd, "Connection && Settings", 22, 14, 300, 18);
    add_ctrl(hwnd, "STATIC", "Port:", SS_LEFT, 22, 36, 32, 16, 0);
    add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 56, 34, 112, 160, IDC_PORT_COMBO);
    add_ctrl(hwnd, "BUTTON", "Refresh", BS_OWNERDRAW | WS_TABSTOP, 174, 34, 56, 22, IDC_REFRESH_BTN);
    add_ctrl(hwnd, "BUTTON", "Connect", BS_OWNERDRAW | WS_TABSTOP, 234, 34, 66, 22, IDC_CONNECT_BTN);
    add_ctrl(hwnd, "STATIC", "Disconnected", SS_LEFT, 22, 60, 290, 16, IDC_CONN_STATUS_LBL);

    add_ctrl(hwnd, "STATIC", "Baud:", SS_LEFT, 22, 84, 34, 16, 0);
    add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 58, 82, 90, 140, IDC_BAUD_COMBO);
    add_ctrl(hwnd, "STATIC", "Data Bits:", SS_LEFT, 22, 108, 60, 16, 0);
    add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 86, 106, 45, 100, IDC_DATABITS_COMBO);
    add_ctrl(hwnd, "STATIC", "Parity:", SS_LEFT, 142, 108, 40, 16, 0);
    add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 184, 106, 70, 100, IDC_PARITY_COMBO);
    /* panel bottom = 6 + 138 = 144 */

    /* --- Left column: Module Address & Output (its own panel, combined
     * with each other but not with Connection & Settings or Power) --- */
    add_panel(hwnd, 10, 152, 335, 84);
    add_header(hwnd, "Address && Output", 22, 160, 300, 18);
    add_ctrl(hwnd, "STATIC", "Address:", SS_LEFT, 22, 182, 52, 16, 0);
    add_ctrl(hwnd, "EDIT", "0", WS_BORDER | ES_NUMBER, 76, 180, 50, 20, IDC_ADDR_EDIT);
    add_ctrl(hwnd, "BUTTON", "Query", BS_OWNERDRAW | WS_TABSTOP, 132, 180, 60, 22, IDC_QUERY_ADDR_BTN);
    add_ctrl(hwnd, "BUTTON", "Set", BS_OWNERDRAW | WS_TABSTOP, 198, 180, 50, 22, IDC_SET_ADDR_BTN);
    add_ctrl(hwnd, "BUTTON", "Output ON", BS_AUTOCHECKBOX | WS_TABSTOP, 22, 206, 110, 20, IDC_OUTPUT_CHECK);
    add_ctrl(hwnd, "STATIC", "OFF", SS_CENTER, 150, 206, 50, 20, IDC_OUTPUT_PILL);
    add_ctrl(hwnd, "STATIC", "Freq:", SS_LEFT, 210, 206, 34, 20, 0);
    add_ctrl(hwnd, "STATIC", "-", SS_LEFT, 246, 206, 80, 20, IDC_OUTPUT_FREQ_LBL);
    /* left column bottom = 152 + 84 = 236 */

    /* --- Right column (x=355, w=335): Signal Settings (mode/bandwidth/power only -
     * frequency lives in its own panel below, where it's actually editable) --- */
    add_panel(hwnd, 355, 6, 335, 204);
    add_header(hwnd, "Signal Settings", 367, 14, 300, 18);
    add_ctrl(hwnd, "STATIC", "Mode:", SS_LEFT, 367, 36, 270, 16, 0);
    /* Native radio buttons (BS_AUTORADIOBUTTON) - the system draws the
     * glyph and handles the group check-state itself; WM_CTLCOLORBTN
     * still colors the label text and background to match the theme. */
    add_ctrl(hwnd, "BUTTON", "Pseudo Random Noise", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 367, 54, 220, 18, IDC_RB_WHITE);
    add_ctrl(hwnd, "BUTTON", "Linear Sweep", BS_AUTORADIOBUTTON | WS_TABSTOP, 367, 72, 150, 18, IDC_RB_SWEEP);
    add_ctrl(hwnd, "BUTTON", "Comb Spectrum", BS_AUTORADIOBUTTON | WS_TABSTOP, 367, 90, 150, 18, IDC_RB_COMB);
    add_ctrl(hwnd, "BUTTON", "Continuous Wave", BS_AUTORADIOBUTTON | WS_TABSTOP, 367, 108, 150, 18, IDC_RB_SINGLE);
    CheckDlgButton(hwnd, IDC_RB_WHITE, BST_CHECKED);

    add_ctrl(hwnd, "STATIC", "Bandwidth:", SS_LEFT, 367, 130, 62, 16, 0);
    add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 431, 128, 140, 140, IDC_BW_COMBO);

    add_ctrl(hwnd, "STATIC", "Power:", SS_LEFT, 367, 152, 50, 16, 0);
    add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 421, 150, 110, 100, IDC_POWER_COMBO);

    /* Apply here and the Frequency panel's Apply both send the same
     * combined mode/freq/bandwidth/power frame - having it in both
     * panels means whichever field you just changed, Apply is right
     * there rather than requiring a trip to the other panel. */
    add_ctrl(hwnd, "BUTTON", "Apply", BS_OWNERDRAW | WS_TABSTOP, 367, 174, 80, 26, IDC_APPLY_BTN);
    add_ctrl(hwnd, "BUTTON", "Read Device", BS_OWNERDRAW | WS_TABSTOP, 453, 174, 100, 26, IDC_READ_BTN);
    /* right column bottom = 6 + 204 = 210 */

    /* --- Left column continues: Frequency (below Address & Output at
     * y=244) - each column flows on its own, so no panel ever sits behind
     * a gap sized for the other one. --- */
    add_panel(hwnd, 10, 244, 335, 84);
    add_header(hwnd, "Frequency", 22, 252, 300, 18);
    add_ctrl(hwnd, "STATIC", "Frequency:", SS_LEFT, 22, 274, 64, 16, 0);
    {
        char freq_label[8];
        wsprintfA(freq_label, "%d", DEFAULT_FREQUENCY_MHZ);
        add_ctrl(hwnd, "EDIT", freq_label, WS_BORDER | ES_NUMBER, 90, 272, 55, 20, IDC_FREQ_EDIT);
    }
    add_ctrl(hwnd, "STATIC", "MHz", SS_LEFT, 148, 274, 28, 16, 0);
    add_ctrl(hwnd, "BUTTON", "-", BS_OWNERDRAW | WS_TABSTOP, 180, 272, 24, 20, IDC_FREQ_MINUS_BTN);
    add_ctrl(hwnd, "BUTTON", "+", BS_OWNERDRAW | WS_TABSTOP, 206, 272, 24, 20, IDC_FREQ_PLUS_BTN);
    add_ctrl(hwnd, "BUTTON", "Lock", BS_AUTOCHECKBOX | WS_TABSTOP, 234, 273, 55, 18, IDC_FREQ_LOCK_CHECK);
    add_ctrl(hwnd, "STATIC", "Step:", SS_LEFT, 22, 298, 32, 16, 0);
    add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 56, 296, 80, 100, IDC_STEP_COMBO);
    add_ctrl(hwnd, "BUTTON", "Apply", BS_OWNERDRAW | WS_TABSTOP, 250, 296, 65, 22, IDC_FREQ_APPLY_BTN);

    /* Frequency starts locked - editing it is a real RF-output-affecting
     * change, so it needs a deliberate unlock (see IDC_FREQ_LOCK_CHECK in
     * WM_COMMAND) rather than being editable by default. */
    CheckDlgButton(hwnd, IDC_FREQ_LOCK_CHECK, BST_CHECKED);
    EnableWindow(GetDlgItem(hwnd, IDC_FREQ_EDIT), FALSE);
    EnableWindow(GetDlgItem(hwnd, IDC_FREQ_MINUS_BTN), FALSE);
    EnableWindow(GetDlgItem(hwnd, IDC_FREQ_PLUS_BTN), FALSE);

    /* --- Right column continues: TX / RX (below Signal Settings at
     * y=210, its own column's actual bottom - not the left column's).
     * Height stretched to 110 (vs. the tight-content 84) so this panel's
     * bottom lands on the same y as the left column's Frequency panel -
     * otherwise the empty space below it (down to the reserved warning-
     * banner row) reads as a big dead zone rather than padding inside
     * a bordered box. --- */
    add_panel(hwnd, 355, 218, 335, 110);
    add_header(hwnd, "TX / RX", 367, 226, 300, 18);
    add_ctrl(hwnd, "STATIC", "TX:", SS_LEFT, 367, 248, 26, 16, 0);
    {
        HWND tx = add_ctrl(hwnd, "EDIT", "", WS_BORDER | ES_READONLY, 393, 246, 270, 20, IDC_TX_EDIT);
        if (tx) SendMessageA(tx, WM_SETFONT, (WPARAM)g_mono_font, TRUE);
    }
    add_ctrl(hwnd, "STATIC", "RX:", SS_LEFT, 367, 272, 26, 16, 0);
    {
        HWND rx = add_ctrl(hwnd, "EDIT", "", WS_BORDER | ES_READONLY, 393, 270, 270, 20, IDC_RX_EDIT);
        if (rx) SendMessageA(rx, WM_SETFONT, (WPARAM)g_mono_font, TRUE);
    }
    /* left column bottom = 244 + 84 = 328; right column bottom = 218 + 110 = 328 */

    /* Warning banner lives below both columns, on the page rather than
     * inside any panel, so it adds zero space when hidden - it only
     * claims a row when there's actually something to say. */
    add_ctrl(hwnd, "STATIC", "", SS_LEFT, 10, 336, 680, 32, IDC_WARNING_LBL);
    ShowWindow(GetDlgItem(hwnd, IDC_WARNING_LBL), SW_HIDE);

    /* ---- populate lists ---- */

    for (i = 0; i < BAUD_OPTIONS_COUNT; i++) {
        char label[16];
        wsprintfA(label, "%d", BAUD_OPTIONS[i]);
        SendDlgItemMessageA(hwnd, IDC_BAUD_COMBO, CB_ADDSTRING, 0, (LPARAM)label);
        SendDlgItemMessageA(hwnd, IDC_BAUD_COMBO, CB_SETITEMDATA, i, (LPARAM)BAUD_OPTIONS[i]);
    }
    SendDlgItemMessageA(hwnd, IDC_BAUD_COMBO, CB_SETCURSEL, BAUD_DEFAULT_INDEX, 0);

    for (i = 0; i < DATABITS_OPTIONS_COUNT; i++) {
        char label[4];
        wsprintfA(label, "%d", DATABITS_OPTIONS[i]);
        SendDlgItemMessageA(hwnd, IDC_DATABITS_COMBO, CB_ADDSTRING, 0, (LPARAM)label);
        SendDlgItemMessageA(hwnd, IDC_DATABITS_COMBO, CB_SETITEMDATA, i, (LPARAM)DATABITS_OPTIONS[i]);
    }
    SendDlgItemMessageA(hwnd, IDC_DATABITS_COMBO, CB_SETCURSEL, DATABITS_DEFAULT_INDEX, 0);

    for (i = 0; i < PARITY_OPTIONS_COUNT; i++) {
        SendDlgItemMessageA(hwnd, IDC_PARITY_COMBO, CB_ADDSTRING, 0, (LPARAM)PARITY_LABELS[i]);
        SendDlgItemMessageA(hwnd, IDC_PARITY_COMBO, CB_SETITEMDATA, i, (LPARAM)(int)PARITY_CODES[i]);
    }
    SendDlgItemMessageA(hwnd, IDC_PARITY_COMBO, CB_SETCURSEL, 0, 0);

    for (i = 0; i < BANDWIDTH_OPTIONS_COUNT; i++) {
        char label[24];
        if (BANDWIDTH_OPTIONS[i] == BANDWIDTH_UNCONFIRMED_MHZ) {
            wsprintfA(label, "%d MHz (unconfirmed)", BANDWIDTH_OPTIONS[i]);
        } else {
            wsprintfA(label, "%d MHz", BANDWIDTH_OPTIONS[i]);
        }
        SendDlgItemMessageA(hwnd, IDC_BW_COMBO, CB_ADDSTRING, 0, (LPARAM)label);
        SendDlgItemMessageA(hwnd, IDC_BW_COMBO, CB_SETITEMDATA, i, (LPARAM)BANDWIDTH_OPTIONS[i]);
    }
    SendDlgItemMessageA(hwnd, IDC_BW_COMBO, CB_SETCURSEL, BANDWIDTH_DEFAULT_INDEX, 0);

    for (i = 0; i < POWER_OPTIONS_COUNT; i++) {
        char label[16];
        if (POWER_OPTIONS[i] == 0) {
            wsprintfA(label, "0 dB (max)");
        } else {
            wsprintfA(label, "%d dB", POWER_OPTIONS[i]);
        }
        SendDlgItemMessageA(hwnd, IDC_POWER_COMBO, CB_ADDSTRING, 0, (LPARAM)label);
        SendDlgItemMessageA(hwnd, IDC_POWER_COMBO, CB_SETITEMDATA, i, (LPARAM)POWER_OPTIONS[i]);
    }
    SendDlgItemMessageA(hwnd, IDC_POWER_COMBO, CB_SETCURSEL, 0, 0);

    for (i = 0; i < STEP_OPTIONS_COUNT; i++) {
        char label[16];
        wsprintfA(label, "%d MHz", STEP_OPTIONS[i]);
        SendDlgItemMessageA(hwnd, IDC_STEP_COMBO, CB_ADDSTRING, 0, (LPARAM)label);
        SendDlgItemMessageA(hwnd, IDC_STEP_COMBO, CB_SETITEMDATA, i, (LPARAM)STEP_OPTIONS[i]);
    }
    SendDlgItemMessageA(hwnd, IDC_STEP_COMBO, CB_SETCURSEL, STEP_DEFAULT_INDEX, 0);
}

/* ---- window procedure ---- */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            ConnectionCallbacks ccb;
            DeviceCallbacks dcb;

            g_hwnd = hwnd;
            g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            g_mono_font = CreateFontA(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                       ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, "Courier New");
            if (!g_mono_font) {
                g_mono_font = g_font;
            }

            /* Bold variant of the same face/size as g_font, used for
             * section title labels (see add_header()). */
            {
                LOGFONTA lf;
                if (GetObjectA(g_font, sizeof(lf), &lf)) {
                    lf.lfWeight = FW_BOLD;
                    g_header_font = CreateFontIndirectA(&lf);
                }
            }
            if (!g_header_font) {
                g_header_font = g_font;
            }

            build_controls(hwnd);
            refresh_port_list();

            memset(&ccb, 0, sizeof(ccb));
            ccb.on_connected_changed = conn_on_connected_changed;
            ccb.on_frame = conn_on_frame;
            ccb.on_raw_tx = conn_on_raw_tx;
            ccb.on_raw_rx = conn_on_raw_rx;
            ccb.on_error = conn_on_error;
            conn_init(&g_conn, ccb);

            memset(&dcb, 0, sizeof(dcb));
            dcb.on_state_changed = dev_on_state_changed;
            dcb.on_command_timeout = dev_on_command_timeout;
            dcb.on_command_failed = dev_on_command_failed;
            device_init(&g_device, &g_conn, dcb);

            ui_refresh_status();
            SetTimer(hwnd, ID_POLL_TIMER, 50, NULL);
            return 0;
        }

        case WM_ERASEBKGND: {
            HDC hdc = (HDC)wParam;
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, g_brush_page);
            draw_dot_grid(hdc, &rc);
            return 1;
        }

        case WM_TIMER:
            if (wParam == ID_POLL_TIMER) {
                conn_poll(&g_conn);
                device_poll_timeout(&g_device);
            }
            return 0;

        case WM_COMMAND: {
            WORD id = LOWORD(wParam);
            WORD code = HIWORD(wParam);
            if (code != BN_CLICKED) {
                break;
            }
            switch (id) {
                case IDC_REFRESH_BTN: refresh_port_list(); break;
                case IDC_CONNECT_BTN: on_connect_clicked(); break;
                case IDC_QUERY_ADDR_BTN: device_query_address(&g_device); break;
                case IDC_SET_ADDR_BTN: {
                    int addr = (int)GetDlgItemInt(hwnd, IDC_ADDR_EDIT, NULL, FALSE);
                    if (addr < PROTO_ADDR_MIN) addr = PROTO_ADDR_MIN;
                    if (addr > PROTO_ADDR_MAX) addr = PROTO_ADDR_MAX;
                    device_set_address(&g_device, (uint8_t)addr);
                    break;
                }
                case IDC_OUTPUT_CHECK: {
                    /* BS_AUTOCHECKBOX already flipped the visible check state
                     * before this message arrives, so "checked" here is the
                     * requested new state. */
                    bool checked = (IsDlgButtonChecked(hwnd, IDC_OUTPUT_CHECK) == BST_CHECKED);
                    const char *prompt = checked ? "Turn device output ON?" : "Turn device output OFF?";
                    if (MessageBoxA(hwnd, prompt, "Confirm", MB_YESNO | MB_ICONWARNING) != IDYES) {
                        CheckDlgButton(hwnd, IDC_OUTPUT_CHECK, checked ? BST_UNCHECKED : BST_CHECKED);
                        break;
                    }
                    if (checked) device_turn_output_on(&g_device);
                    else device_turn_output_off(&g_device);
                    break;
                }
                case IDC_FREQ_MINUS_BTN: step_frequency(-1); break;
                case IDC_FREQ_PLUS_BTN: step_frequency(1); break;
                case IDC_FREQ_LOCK_CHECK: {
                    /* Locking (the safe direction) needs no confirmation;
                     * unlocking - about to allow changes to a live RF
                     * output's frequency - does. */
                    bool locked = (IsDlgButtonChecked(hwnd, IDC_FREQ_LOCK_CHECK) == BST_CHECKED);
                    if (!locked) {
                        if (MessageBoxA(hwnd,
                                         "Unlock frequency for editing?\n\n"
                                         "Misconfiguration or an excessively high frequency "
                                         "can cause hardware damage.",
                                         "Confirm", MB_YESNO | MB_ICONWARNING) != IDYES) {
                            CheckDlgButton(hwnd, IDC_FREQ_LOCK_CHECK, BST_CHECKED);
                            locked = true;
                        }
                    }
                    EnableWindow(GetDlgItem(hwnd, IDC_FREQ_EDIT), !locked);
                    EnableWindow(GetDlgItem(hwnd, IDC_FREQ_MINUS_BTN), !locked);
                    EnableWindow(GetDlgItem(hwnd, IDC_FREQ_PLUS_BTN), !locked);
                    break;
                }
                case IDC_APPLY_BTN:
                case IDC_FREQ_APPLY_BTN: {
                    if (show_confirm_dialog(hwnd,
                                             "WARNING: Incorrect frequency settings can "
                                             "damage your RF Amplifier.")) {
                        on_apply_clicked();
                    }
                    break;
                }
                case IDC_READ_BTN: device_read_status(&g_device); break;
                default: break;
            }
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            HWND ctl = (HWND)lParam;
            HDC hdc = (HDC)wParam;
            if (ctl == GetDlgItem(hwnd, IDC_CONN_STATUS_LBL)) {
                SetTextColor(hdc, g_device.state.connected ? COLOR_APP_CONNECTED : COLOR_APP_DISCONNECTED);
                SetBkMode(hdc, TRANSPARENT);
                return (LRESULT)g_brush_panel;
            }
            if (ctl == GetDlgItem(hwnd, IDC_OUTPUT_PILL)) {
                SetTextColor(hdc, g_device.state.output_on ? COLOR_APP_ACCENT : COLOR_APP_MUTED);
                SetBkMode(hdc, TRANSPARENT);
                return (LRESULT)g_brush_panel;
            }
            if (ctl == GetDlgItem(hwnd, IDC_WARNING_LBL)) {
                if (!g_brush_warn) {
                    g_brush_warn = CreateSolidBrush(RGB(254, 243, 199));
                }
                SetTextColor(hdc, RGB(146, 64, 14));
                SetBkColor(hdc, RGB(254, 243, 199));
                return (LRESULT)g_brush_warn;
            }
            /* TX/RX are ES_READONLY, so they land here rather than
             * WM_CTLCOLOREDIT - a light grey field reads more like a
             * terminal/log readout against the dark theme. */
            if (ctl == GetDlgItem(hwnd, IDC_TX_EDIT) || ctl == GetDlgItem(hwnd, IDC_RX_EDIT)) {
                SetTextColor(hdc, RGB(30, 31, 33));
                SetBkColor(hdc, RGB(230, 231, 233));
                SetBkMode(hdc, OPAQUE);
                return (LRESULT)g_brush_light;
            }
            /* Section title labels use the bold header font - recognized
             * here (rather than by id) so add_header() is the only place
             * that needs to know about it - and get the accent color. Every
             * other plain label, including the empty-text panel rectangles
             * from add_panel(), is charcoal text on the panel fill. */
            if ((HFONT)SendMessageA(ctl, WM_GETFONT, 0, 0) == g_header_font) {
                SetTextColor(hdc, COLOR_APP_HEADER);
            } else {
                SetTextColor(hdc, COLOR_APP_TEXT);
            }
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)g_brush_panel;
        }

        case WM_CTLCOLORBTN: {
            /* Checkboxes and radio buttons are BUTTON-class controls that
             * aren't owner-drawn (only push buttons are), so they land
             * here rather than WM_CTLCOLORSTATIC. */
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, COLOR_APP_TEXT);
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)g_brush_panel;
        }

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            /* Editable edit boxes and combo box list areas. TX/RX are
             * ES_READONLY, so they never reach here - see WM_CTLCOLORSTATIC
             * instead. */
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, COLOR_APP_TEXT);
            SetBkColor(hdc, COLOR_APP_FIELD_BG);
            SetBkMode(hdc, OPAQUE);
            return (LRESULT)g_brush_field;
        }

        case WM_DRAWITEM: {
            DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lParam;
            if (dis->CtlType == ODT_BUTTON) {
                char text[64];
                bool disabled = (dis->itemState & ODS_DISABLED) != 0;
                RECT rc = dis->rcItem;
                FillRect(dis->hDC, &rc, disabled ? g_brush_accent_dis : g_brush_accent);
                SetTextColor(dis->hDC, RGB(255, 255, 255));
                SetBkMode(dis->hDC, TRANSPARENT);
                GetWindowTextA(dis->hwndItem, text, sizeof(text));
                DrawTextA(dis->hDC, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                if (dis->itemState & ODS_FOCUS) {
                    RECT focus_rc = rc;
                    InflateRect(&focus_rc, -3, -3);
                    DrawFocusRect(dis->hDC, &focus_rc);
                }
                return TRUE;
            }
            break;
        }

        case WM_DESTROY:
            KillTimer(hwnd, ID_POLL_TIMER);
            if (conn_is_connected(&g_conn)) {
                conn_disconnect(&g_conn);
            }
            if (g_brush_warn) {
                DeleteObject(g_brush_warn);
            }
            if (g_brush_panel) {
                DeleteObject(g_brush_panel);
            }
            if (g_brush_page) {
                DeleteObject(g_brush_page);
            }
            if (g_brush_field) {
                DeleteObject(g_brush_field);
            }
            if (g_brush_accent) {
                DeleteObject(g_brush_accent);
            }
            if (g_brush_accent_dis) {
                DeleteObject(g_brush_accent_dis);
            }
            if (g_brush_dot) {
                DeleteObject(g_brush_dot);
            }
            if (g_brush_silver) {
                DeleteObject(g_brush_silver);
            }
            if (g_brush_light) {
                DeleteObject(g_brush_light);
            }
            if (g_mono_font && g_mono_font != g_font) {
                DeleteObject(g_mono_font);
            }
            if (g_header_font && g_header_font != g_font) {
                DeleteObject(g_header_font);
            }
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    WNDCLASSEXA wc;
    RECT rect;
    HWND hwnd;
    MSG msg;

    (void)hPrevInstance;
    (void)lpCmdLine;

    g_hinst = hInstance;

    /* Created before the window class registers, since hbrBackground needs
     * a real brush up front; freed in WM_DESTROY. */
    g_brush_page = CreateSolidBrush(COLOR_APP_PAGE_BG);
    g_brush_panel = CreateSolidBrush(COLOR_APP_PANEL_BG);
    g_brush_field = CreateSolidBrush(COLOR_APP_FIELD_BG);
    g_brush_accent = CreateSolidBrush(COLOR_APP_ACCENT);
    g_brush_accent_dis = CreateSolidBrush(COLOR_APP_ACCENT_DIS);
    g_brush_dot = CreateSolidBrush(COLOR_APP_DOT);
    g_brush_silver = CreateSolidBrush(COLOR_APP_SILVER);
    g_brush_light = CreateSolidBrush(RGB(230, 231, 233));

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = (HICON)LoadImageA(hInstance, MAKEINTRESOURCEA(IDI_APP_ICON), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
    wc.hIconSm = (HICON)LoadImageA(hInstance, MAKEINTRESOURCEA(IDI_APP_ICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    if (!wc.hIcon) {
        wc.hIcon = LoadIconA(NULL, IDI_APPLICATION);
    }
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = g_brush_page;
    wc.lpszClassName = "TxLiteMainWindow";
    RegisterClassExA(&wc);

    rect.left = 0;
    rect.top = 0;
    rect.right = CLIENT_WIDTH;
    rect.bottom = CLIENT_HEIGHT;
    AdjustWindowRectEx(&rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);

    hwnd = CreateWindowExA(0, "TxLiteMainWindow", "TX Lite",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            CW_USEDEFAULT, CW_USEDEFAULT,
                            rect.right - rect.left, rect.bottom - rect.top,
                            NULL, NULL, hInstance, NULL);
    if (!hwnd) {
        return 0;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}
