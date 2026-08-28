#include "device.h"
#include <string.h>

/* Plain ASCII placeholder for "no value yet" - the Python reference uses an
 * em dash ("—"), but that needs UTF-8-to-codepage handling to render
 * correctly through the ANSI Win32 API this app uses everywhere else, which
 * isn't worth it for one cosmetic character. */
static const char UNKNOWN_TEXT[] = "-";

void device_init(Device *dev, Connection *conn, DeviceCallbacks cb) {
    memset(dev, 0, sizeof(*dev));
    dev->conn = conn;
    dev->cb = cb;
    dev->state.mode = DEVICE_UNKNOWN;
    dev->state.frequency_mhz = DEVICE_UNKNOWN;
    dev->state.bandwidth_mhz = DEVICE_UNKNOWN;
    dev->state.power_db = DEVICE_UNKNOWN;
    lstrcpynA(dev->state.last_command, UNKNOWN_TEXT, (int)sizeof(dev->state.last_command));
}

static void device_notify(Device *dev) {
    if (dev->cb.on_state_changed) {
        dev->cb.on_state_changed(dev->cb.ctx);
    }
}

static void device_cancel_pending(Device *dev) {
    dev->pending.active = false;
    dev->pending.kind = PENDING_NONE;
}

static void device_set_pending(Device *dev, PendingKind kind, const char *label) {
    dev->pending.active = true;
    dev->pending.kind = kind;
    dev->pending.deadline_tick = GetTickCount() + DEVICE_RESPONSE_TIMEOUT_MS;
    lstrcpynA(dev->pending.label, label, (int)sizeof(dev->pending.label));
}

static void device_send_output(Device *dev, bool on) {
    bool previous = dev->state.output_on;
    ProtoFrame frame;
    const char *label = on ? "Output ON" : "Output OFF";

    proto_build_output_switch(&frame, dev->state.address, on);

    /* Optimistic: flip immediately so the toggle doesn't visually lag
     * behind the click, reverted below if the send fails outright, or
     * later (device_on_frame / device_poll_timeout) if the device rejects
     * it or never responds. */
    dev->state.output_on = on;
    lstrcpynA(dev->state.last_command, label, (int)sizeof(dev->state.last_command));

    if (!conn_send(dev->conn, frame.data, frame.len)) {
        dev->state.output_on = previous;
        device_notify(dev);
        return;
    }

    device_set_pending(dev, PENDING_OUTPUT, label);
    dev->pending.output_new = on;
    dev->pending.output_prev = previous;
    device_notify(dev);
}

void device_turn_output_on(Device *dev) { device_send_output(dev, true); }
void device_turn_output_off(Device *dev) { device_send_output(dev, false); }

ProtoStatus device_apply_signal_settings(Device *dev, uint8_t mode, uint16_t freq_mhz,
                                          uint16_t bandwidth_mhz, int power_db) {
    ProtoFrame frame;
    ProtoStatus status = proto_build_signal_control(&frame, dev->state.address, mode,
                                                      freq_mhz, bandwidth_mhz, power_db);
    char label[80];

    if (status != PROTO_OK) {
        return status;
    }

    wsprintfA(label, "Signal: mode=%u f=%uMHz bw=%uMHz p=%ddB", mode, freq_mhz, bandwidth_mhz, power_db);
    lstrcpynA(dev->state.last_command, label, (int)sizeof(dev->state.last_command));

    /* Unlike output, signal settings are not applied optimistically - the
     * displayed values only change once the device ACKs, matching the
     * Python reference (apply_signal_settings passes no revert_update
     * because it never touches state ahead of confirmation). */
    if (conn_send(dev->conn, frame.data, frame.len)) {
        device_set_pending(dev, PENDING_SIGNAL, label);
        dev->pending.sig_mode = mode;
        dev->pending.sig_freq = freq_mhz;
        dev->pending.sig_bw = bandwidth_mhz;
        dev->pending.sig_power = power_db;
    }
    device_notify(dev);
    return PROTO_OK;
}

void device_read_status(Device *dev) {
    ProtoFrame frame;
    proto_build_status_query(&frame, dev->state.address);
    lstrcpynA(dev->state.last_command, "Status query", (int)sizeof(dev->state.last_command));
    if (conn_send(dev->conn, frame.data, frame.len)) {
        device_set_pending(dev, PENDING_QUERY, "Status query");
    }
    device_notify(dev);
}

void device_query_address(Device *dev) {
    ProtoFrame frame;
    proto_build_addr_query(&frame);
    lstrcpynA(dev->state.last_command, "Query address", (int)sizeof(dev->state.last_command));
    if (conn_send(dev->conn, frame.data, frame.len)) {
        device_set_pending(dev, PENDING_QUERY, "Query address");
    }
    device_notify(dev);
}

void device_set_address(Device *dev, uint8_t new_addr) {
    ProtoFrame frame;
    char label[40];

    /* build_addr_set only rejects addresses outside 0-199; the Win32
     * address control clamps to that same range (like the reference's
     * QSpinBox(0, 199)), so this is a defensive no-op in practice. */
    if (proto_build_addr_set(&frame, new_addr) != PROTO_OK) {
        return;
    }

    wsprintfA(label, "Set address to %u", new_addr);
    lstrcpynA(dev->state.last_command, label, (int)sizeof(dev->state.last_command));
    if (conn_send(dev->conn, frame.data, frame.len)) {
        device_set_pending(dev, PENDING_ADDR_SET, label);
        dev->pending.addr_new = new_addr;
    }
    device_notify(dev);
}

void device_on_frame(const ProtoParsedFrame *frame, void *ctx) {
    Device *dev = (Device *)ctx;
    /* Snapshot before cancelling: any received frame clears whatever
     * timeout is pending, regardless of whether this frame is actually
     * the response to it - matching the Python reference exactly. */
    PendingCommand pending = dev->pending;
    device_cancel_pending(dev);

    if ((frame->type == PROTO_TYPE_OUTPUT_SWITCH || frame->type == PROTO_TYPE_SIGNAL_CONTROL ||
         frame->type == PROTO_TYPE_ADDR_SET) && frame->buf_len == 1) {
        uint8_t code = frame->buf[0];
        if (code == PROTO_RESP_SUCCESS) {
            if (pending.active) {
                switch (pending.kind) {
                    case PENDING_OUTPUT:
                        dev->state.output_on = pending.output_new;
                        break;
                    case PENDING_SIGNAL:
                        dev->state.mode = pending.sig_mode;
                        dev->state.frequency_mhz = pending.sig_freq;
                        dev->state.bandwidth_mhz = pending.sig_bw;
                        dev->state.power_db = pending.sig_power;
                        break;
                    case PENDING_ADDR_SET:
                        dev->state.address = pending.addr_new;
                        dev->state.address_known = true;
                        break;
                    default:
                        break;
                }
            }
        } else if (code == PROTO_RESP_FAILED) {
            char msg[128];
            if (pending.active && pending.label[0]) {
                wsprintfA(msg, "Device rejected command: %s", pending.label);
            } else {
                lstrcpynA(msg, "Device rejected command", (int)sizeof(msg));
            }
            if (pending.active && pending.kind == PENDING_OUTPUT) {
                dev->state.output_on = pending.output_prev;
            }
            if (dev->cb.on_command_failed) {
                dev->cb.on_command_failed(msg, dev->cb.ctx);
            }
        }
    } else if (frame->type == PROTO_TYPE_STATUS_QUERY && frame->buf_len >= 6) {
        uint8_t output = frame->buf[0];
        uint8_t mode = frame->buf[1];
        uint16_t freq = (uint16_t)(((uint16_t)frame->buf[2] << 8) | frame->buf[3]);
        uint8_t bw_code = frame->buf[4];
        uint8_t pw_code = frame->buf[5];
        int bw_mhz = proto_bandwidth_mhz(bw_code);
        int pw_db = proto_power_db(pw_code);

        dev->state.output_on = (output != 0);
        dev->state.mode = mode;
        dev->state.frequency_mhz = freq;
        dev->state.bandwidth_mhz = (bw_mhz >= 0) ? bw_mhz : DEVICE_UNKNOWN;
        dev->state.power_db = (pw_db != -1) ? pw_db : DEVICE_UNKNOWN;
    } else if (frame->type == PROTO_TYPE_ADDR_QUERY && frame->buf_len == 1) {
        dev->state.address = frame->buf[0];
        dev->state.address_known = true;
    }

    device_notify(dev);
}

void device_on_connected_changed(bool connected, void *ctx) {
    Device *dev = (Device *)ctx;
    dev->state.connected = connected;
    /* No auto-query on connect - the address (and everything else) stays
     * "-" until the user explicitly hits Query/Read, rather than silently
     * populating itself the moment a connection opens. */
    device_notify(dev);
}

void device_poll_timeout(Device *dev) {
    char msg[192];

    if (!dev->pending.active) {
        return;
    }
    /* Wraparound-safe "has the deadline passed" check for GetTickCount(). */
    if ((int32_t)(GetTickCount() - dev->pending.deadline_tick) < 0) {
        return;
    }

    wsprintfA(msg,
              "No response within %dms for: %s (could be wiring, module power, "
              "module address, connection settings, or a software issue on either side)",
              DEVICE_RESPONSE_TIMEOUT_MS, dev->pending.label);

    if (dev->pending.kind == PENDING_OUTPUT) {
        dev->state.output_on = dev->pending.output_prev;
    }
    device_cancel_pending(dev);

    if (dev->cb.on_command_timeout) {
        dev->cb.on_command_timeout(msg, dev->cb.ctx);
    }
    device_notify(dev);
}
