#pragma once

#define IDI_APP_ICON         100

#define IDC_PORT_COMBO       1001
#define IDC_REFRESH_BTN      1002
#define IDC_CONNECT_BTN      1003
#define IDC_CONN_STATUS_LBL  1004
#define IDC_BAUD_COMBO       1005
#define IDC_DATABITS_COMBO   1006
#define IDC_PARITY_COMBO     1007

#define IDC_WARNING_LBL      1008

#define ID_POLL_TIMER        1

/* Each of the 16 channel cards gets 3 controls (Mode combo, Level combo,
 * status label) at IDC_CH_BASE + channel_index*IDC_CH_STRIDE + offset,
 * rather than 48 separate #defines. */
#define IDC_CH_BASE          2000
#define IDC_CH_STRIDE        10
#define IDC_CH_MODE_OFFSET   0
#define IDC_CH_LEVEL_OFFSET  1
#define IDC_CH_STATUS_OFFSET 2
