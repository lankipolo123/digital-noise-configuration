/* Device state + command orchestration on top of Connection. Ported from
 * sdr_controller's DeviceController + DeviceState: blind commands are sent,
 * a response is expected within RESPONSE_TIMEOUT_MS, and the pending
 * command's outcome (ACK/NAK/timeout) drives whether state actually
 * changes. Output toggling is optimistic (state flips immediately, reverts
 * on rejection/timeout); signal/address changes only take effect on ACK.
 * Any received frame cancels whatever timeout is pending, and status/address
 * query responses update state from their own payload regardless of what
 * was pending - this matches the Python reference's behavior exactly.
 */
#pragma once
#include "connection.h"
#include "protocol.h"
#include <stdint.h>
#include <stdbool.h>

#define DEVICE_RESPONSE_TIMEOUT_MS 2000
#define DEVICE_UNKNOWN (-1)

typedef struct {
    bool connected;
    uint8_t address;
    bool output_on;
    int mode;          /* DEVICE_UNKNOWN or PROTO_MODE_* */
    int frequency_mhz; /* DEVICE_UNKNOWN or a real MHz value */
    int bandwidth_mhz; /* DEVICE_UNKNOWN or a real MHz value */
    int power_db;      /* DEVICE_UNKNOWN or a real dB value (0, -6, -12) */
    char last_command[80];
} DeviceState;

typedef enum {
    PENDING_NONE = 0,
    PENDING_OUTPUT,
    PENDING_SIGNAL,
    PENDING_ADDR_SET,
    PENDING_QUERY,
} PendingKind;

typedef struct {
    bool active;
    PendingKind kind;
    DWORD deadline_tick;
    char label[80];

    bool output_new;
    bool output_prev;

    int sig_mode, sig_freq, sig_bw, sig_power;

    uint8_t addr_new;
} PendingCommand;

typedef struct Device Device;

typedef struct {
    void (*on_state_changed)(void *ctx);
    void (*on_command_timeout)(const char *message, void *ctx);
    void (*on_command_failed)(const char *message, void *ctx);
    void *ctx;
} DeviceCallbacks;

struct Device {
    Connection *conn;
    DeviceState state;
    PendingCommand pending;
    DeviceCallbacks cb;
};

void device_init(Device *dev, Connection *conn, DeviceCallbacks cb);

void device_turn_output_on(Device *dev);
void device_turn_output_off(Device *dev);
/* Returns the ProtoStatus from proto_build_signal_control so the caller can
 * show a validation error without sending anything - e.g. mode/frequency/
 * bandwidth/power out of range. PROTO_OK means the command was sent. */
ProtoStatus device_apply_signal_settings(Device *dev, uint8_t mode, uint16_t freq_mhz,
                                          uint16_t bandwidth_mhz, int power_db);
void device_read_status(Device *dev);
void device_query_address(Device *dev);
void device_set_address(Device *dev, uint8_t new_addr);

/* Call once per frame received (wired as the Connection's on_frame
 * callback) and once per timer tick (to notice an expired pending
 * command). */
void device_on_frame(const ProtoParsedFrame *frame, void *ctx);
void device_on_connected_changed(bool connected, void *ctx);
void device_poll_timeout(Device *dev);
