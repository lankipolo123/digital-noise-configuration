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
#define CLIENT_HEIGHT 380

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

/* MILITRONIX palette: light cool-gray chrome, charcoal text, flat blue
 * accent on buttons - matching the logo's flat geometric look (solid
 * fills, no gradients, no theming). */
#define COLOR_APP_BG        RGB(237, 241, 242)
#define COLOR_APP_TEXT      RGB(64, 64, 66)
#define COLOR_APP_ACCENT    RGB(13, 110, 158)
#define COLOR_APP_ACCENT_DIS RGB(180, 195, 205)
#define COLOR_APP_FIELD_BG  RGB(255, 255, 255)

static HINSTANCE g_hinst;
static HWND g_hwnd;
static HFONT g_font;
static HFONT g_mono_font;
static HBRUSH g_brush_warn;
static HBRUSH g_brush_bg;
static HBRUSH g_brush_field;
static HBRUSH g_brush_accent;
static HBRUSH g_brush_accent_dis;

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
    char buf[64];
    int pos = 0, i;
    (void)ctx;
    for (i = 0; i < len && pos < (int)sizeof(buf) - 4; i++) {
        pos += wsprintfA(buf + pos, i ? " %02X" : "%02X", data[i]);
    }
    buf[pos] = '\0';
    SetDlgItemTextA(g_hwnd, IDC_TX_EDIT, buf);
}

static void conn_on_raw_rx(const uint8_t *data, uint16_t len, void *ctx) {
    char buf[196];
    int pos = 0, i;
    int n = (len > 64) ? 64 : (int)len; /* real frames are tiny; this just bounds a garbage burst */
    (void)ctx;
    for (i = 0; i < n && pos < (int)sizeof(buf) - 4; i++) {
        pos += wsprintfA(buf + pos, i ? " %02X" : "%02X", data[i]);
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
    bool mode_unconfirmed, bw_unconfirmed;

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

    mode_unconfirmed = (mode == PROTO_MODE_SINGLE);
    bw_unconfirmed = (bw_mhz == BANDWIDTH_UNCONFIRMED_MHZ);

    if (mode_unconfirmed || bw_unconfirmed) {
        char msg[256];
        const char *what = (mode_unconfirmed && bw_unconfirmed) ? "modulation mode and bandwidth"
                            : mode_unconfirmed ? "modulation mode" : "bandwidth";
        wsprintfA(msg,
                  "The selected %s uses a guessed protocol byte value that hasn't been "
                  "verified against real hardware. Send anyway?", what);
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

    /* --- Left column (x=10, w=335): Connection & Settings --- */
    add_ctrl(hwnd, "BUTTON", "Connection && Settings", BS_GROUPBOX, 10, 6, 335, 140, 0);
    add_ctrl(hwnd, "STATIC", "Port:", SS_LEFT, 22, 26, 32, 16, 0);
    add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 56, 24, 112, 160, IDC_PORT_COMBO);
    add_ctrl(hwnd, "BUTTON", "Refresh", BS_OWNERDRAW | WS_TABSTOP, 174, 24, 56, 22, IDC_REFRESH_BTN);
    add_ctrl(hwnd, "BUTTON", "Connect", BS_OWNERDRAW | WS_TABSTOP, 234, 24, 66, 22, IDC_CONNECT_BTN);
    add_ctrl(hwnd, "STATIC", "Disconnected", SS_LEFT, 22, 54, 290, 16, IDC_CONN_STATUS_LBL);

    add_ctrl(hwnd, "STATIC", "Baud:", SS_LEFT, 22, 82, 34, 16, 0);
    add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 58, 80, 90, 140, IDC_BAUD_COMBO);
    add_ctrl(hwnd, "STATIC", "Data Bits:", SS_LEFT, 22, 110, 60, 16, 0);
    add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 86, 108, 45, 100, IDC_DATABITS_COMBO);
    add_ctrl(hwnd, "STATIC", "Parity:", SS_LEFT, 142, 110, 40, 16, 0);
    add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 184, 108, 70, 100, IDC_PARITY_COMBO);

    /* --- Left column: Module Address & Output (combined) --- */
    add_ctrl(hwnd, "BUTTON", "Address && Output", BS_GROUPBOX, 10, 154, 335, 84, 0);
    add_ctrl(hwnd, "STATIC", "Address:", SS_LEFT, 22, 176, 52, 16, 0);
    add_ctrl(hwnd, "EDIT", "0", WS_BORDER | ES_NUMBER, 76, 174, 50, 20, IDC_ADDR_EDIT);
    add_ctrl(hwnd, "BUTTON", "Query", BS_OWNERDRAW | WS_TABSTOP, 132, 174, 60, 22, IDC_QUERY_ADDR_BTN);
    add_ctrl(hwnd, "BUTTON", "Set", BS_OWNERDRAW | WS_TABSTOP, 198, 174, 50, 22, IDC_SET_ADDR_BTN);
    add_ctrl(hwnd, "BUTTON", "Output ON", BS_AUTOCHECKBOX | WS_TABSTOP, 22, 204, 110, 20, IDC_OUTPUT_CHECK);
    add_ctrl(hwnd, "STATIC", "OFF", SS_CENTER, 150, 204, 50, 20, IDC_OUTPUT_PILL);
    add_ctrl(hwnd, "STATIC", "Freq:", SS_LEFT, 210, 204, 34, 20, 0);
    add_ctrl(hwnd, "STATIC", "-", SS_LEFT, 246, 204, 80, 20, IDC_OUTPUT_FREQ_LBL);
    /* left column bottom = 154 + 84 = 238 */

    /* --- Right column (x=355, w=335): Signal Settings (mode/bandwidth/power only -
     * frequency now lives in its own box below, where it's actually editable) --- */
    add_ctrl(hwnd, "BUTTON", "Signal Settings", BS_GROUPBOX, 355, 6, 335, 212, 0);
    add_ctrl(hwnd, "STATIC", "Mode:", SS_LEFT, 367, 26, 270, 16, 0);
    add_ctrl(hwnd, "BUTTON", "White Noise", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 367, 44, 150, 18, IDC_RB_WHITE);
    add_ctrl(hwnd, "BUTTON", "Linear Sweep", BS_AUTORADIOBUTTON | WS_TABSTOP, 367, 62, 150, 18, IDC_RB_SWEEP);
    add_ctrl(hwnd, "BUTTON", "Comb Spectrum", BS_AUTORADIOBUTTON | WS_TABSTOP, 367, 80, 150, 18, IDC_RB_COMB);
    add_ctrl(hwnd, "BUTTON", "Single (unconfirmed)", BS_AUTORADIOBUTTON | WS_TABSTOP, 367, 98, 190, 18, IDC_RB_SINGLE);
    CheckDlgButton(hwnd, IDC_RB_WHITE, BST_CHECKED);

    add_ctrl(hwnd, "STATIC", "Bandwidth:", SS_LEFT, 367, 124, 62, 16, 0);
    add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 431, 122, 140, 140, IDC_BW_COMBO);

    add_ctrl(hwnd, "STATIC", "Power:", SS_LEFT, 367, 150, 50, 16, 0);
    add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 421, 148, 110, 100, IDC_POWER_COMBO);

    add_ctrl(hwnd, "BUTTON", "Apply", BS_OWNERDRAW | WS_TABSTOP, 367, 178, 80, 26, IDC_APPLY_BTN);
    add_ctrl(hwnd, "BUTTON", "Read Device", BS_OWNERDRAW | WS_TABSTOP, 453, 178, 100, 26, IDC_READ_BTN);
    /* right column bottom = 6 + 212 = 218 */

    /* --- Full width below both columns (below y=238/218, the taller of the two):
     * Frequency - the actual editable controls, not a readout --- */
    add_ctrl(hwnd, "BUTTON", "Frequency", BS_GROUPBOX, 10, 246, 335, 78, 0);
    add_ctrl(hwnd, "STATIC", "Frequency:", SS_LEFT, 22, 268, 64, 16, 0);
    {
        char freq_label[8];
        wsprintfA(freq_label, "%d", DEFAULT_FREQUENCY_MHZ);
        add_ctrl(hwnd, "EDIT", freq_label, WS_BORDER | ES_NUMBER, 90, 266, 55, 20, IDC_FREQ_EDIT);
    }
    add_ctrl(hwnd, "STATIC", "MHz", SS_LEFT, 148, 268, 28, 16, 0);
    add_ctrl(hwnd, "BUTTON", "-", BS_OWNERDRAW | WS_TABSTOP, 180, 266, 24, 20, IDC_FREQ_MINUS_BTN);
    add_ctrl(hwnd, "BUTTON", "+", BS_OWNERDRAW | WS_TABSTOP, 206, 266, 24, 20, IDC_FREQ_PLUS_BTN);
    add_ctrl(hwnd, "BUTTON", "Lock", BS_AUTOCHECKBOX | WS_TABSTOP, 234, 267, 55, 18, IDC_FREQ_LOCK_CHECK);
    add_ctrl(hwnd, "STATIC", "Step:", SS_LEFT, 22, 292, 32, 16, 0);
    add_ctrl(hwnd, "COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 56, 290, 80, 100, IDC_STEP_COMBO);

    /* Frequency starts locked - editing it is a real RF-output-affecting
     * change, so it needs a deliberate unlock (see IDC_FREQ_LOCK_CHECK in
     * WM_COMMAND) rather than being editable by default. */
    CheckDlgButton(hwnd, IDC_FREQ_LOCK_CHECK, BST_CHECKED);
    EnableWindow(GetDlgItem(hwnd, IDC_FREQ_EDIT), FALSE);
    EnableWindow(GetDlgItem(hwnd, IDC_FREQ_MINUS_BTN), FALSE);
    EnableWindow(GetDlgItem(hwnd, IDC_FREQ_PLUS_BTN), FALSE);

    /* --- TX / RX (same row as Frequency, right column - same height, same
     * two-row rhythm, so nothing looks lopsided) --- */
    add_ctrl(hwnd, "BUTTON", "TX / RX", BS_GROUPBOX, 355, 246, 335, 78, 0);
    add_ctrl(hwnd, "STATIC", "TX:", SS_LEFT, 367, 268, 26, 16, 0);
    {
        HWND tx = add_ctrl(hwnd, "EDIT", "", WS_BORDER | ES_READONLY, 393, 266, 270, 20, IDC_TX_EDIT);
        if (tx) SendMessageA(tx, WM_SETFONT, (WPARAM)g_mono_font, TRUE);
    }
    add_ctrl(hwnd, "STATIC", "RX:", SS_LEFT, 367, 292, 26, 16, 0);
    {
        HWND rx = add_ctrl(hwnd, "EDIT", "", WS_BORDER | ES_READONLY, 393, 290, 270, 20, IDC_RX_EDIT);
        if (rx) SendMessageA(rx, WM_SETFONT, (WPARAM)g_mono_font, TRUE);
    }

    /* Warning banner lives below both boxes so it adds zero space to
     * either when hidden - it only claims a row when there's actually
     * something to say. */
    add_ctrl(hwnd, "STATIC", "", SS_LEFT, 10, 332, 680, 32, IDC_WARNING_LBL);
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
                        if (MessageBoxA(hwnd, "Unlock frequency for editing?", "Confirm",
                                         MB_YESNO | MB_ICONWARNING) != IDYES) {
                            CheckDlgButton(hwnd, IDC_FREQ_LOCK_CHECK, BST_CHECKED);
                            locked = true;
                        }
                    }
                    EnableWindow(GetDlgItem(hwnd, IDC_FREQ_EDIT), !locked);
                    EnableWindow(GetDlgItem(hwnd, IDC_FREQ_MINUS_BTN), !locked);
                    EnableWindow(GetDlgItem(hwnd, IDC_FREQ_PLUS_BTN), !locked);
                    break;
                }
                case IDC_APPLY_BTN: on_apply_clicked(); break;
                case IDC_READ_BTN: device_read_status(&g_device); break;
                default: break;
            }
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            HWND ctl = (HWND)lParam;
            HDC hdc = (HDC)wParam;
            if (ctl == GetDlgItem(hwnd, IDC_CONN_STATUS_LBL)) {
                SetTextColor(hdc, g_device.state.connected ? RGB(8, 127, 35) : RGB(176, 0, 32));
                SetBkMode(hdc, TRANSPARENT);
                return (LRESULT)g_brush_bg;
            }
            if (ctl == GetDlgItem(hwnd, IDC_OUTPUT_PILL)) {
                SetTextColor(hdc, g_device.state.output_on ? COLOR_APP_ACCENT : RGB(100, 100, 100));
                SetBkMode(hdc, TRANSPARENT);
                return (LRESULT)g_brush_bg;
            }
            if (ctl == GetDlgItem(hwnd, IDC_WARNING_LBL)) {
                if (!g_brush_warn) {
                    g_brush_warn = CreateSolidBrush(RGB(254, 243, 199));
                }
                SetTextColor(hdc, RGB(146, 64, 14));
                SetBkColor(hdc, RGB(254, 243, 199));
                return (LRESULT)g_brush_warn;
            }
            /* Every other plain label: brand text on brand background. */
            SetTextColor(hdc, COLOR_APP_TEXT);
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)g_brush_bg;
        }

        case WM_CTLCOLORBTN: {
            /* Checkboxes, radio buttons, and group box captions are BUTTON-
             * class controls that aren't owner-drawn (only push buttons
             * are), so they land here rather than WM_CTLCOLORSTATIC. */
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, COLOR_APP_TEXT);
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)g_brush_bg;
        }

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            /* Edit boxes and combo box display/list areas: white fields on
             * the brand background read as "input", same convention as the
             * rest of the app's flat, unthemed styling. */
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
            if (g_brush_bg) {
                DeleteObject(g_brush_bg);
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
            if (g_mono_font && g_mono_font != g_font) {
                DeleteObject(g_mono_font);
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
    g_brush_bg = CreateSolidBrush(COLOR_APP_BG);
    g_brush_field = CreateSolidBrush(COLOR_APP_FIELD_BG);
    g_brush_accent = CreateSolidBrush(COLOR_APP_ACCENT);
    g_brush_accent_dis = CreateSolidBrush(COLOR_APP_ACCENT_DIS);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconA(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = g_brush_bg;
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
