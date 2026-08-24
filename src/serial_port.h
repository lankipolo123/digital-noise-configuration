/* Direct Win32 serial I/O - no pyserial, no vendor DLL, no CRT allocation
 * beyond what CreateFile/ReadFile/WriteFile themselves need.
 * Ported from sdr_controller/serial_io/serial_manager.py.
 */
#pragma once
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

#define SERIAL_DEFAULT_BAUD 115200
/* Matches SerialManager's 0.2s pyserial timeout: a read blocks until data
 * arrives or this many ms pass, whichever is first. */
#define SERIAL_READ_TIMEOUT_MS 200

typedef struct {
    HANDLE handle;
} SerialPort;

/* port_name like "COM5" (no leading "\\.\" needed - added internally).
 * parity: 'N','O','E','M','S' (defaults to none on anything else).
 * data_bits: 5-8 (defaults to 8 on anything else). Always 1 stop bit,
 * matching the Python reference (STOPBITS_ONE is the only option used). */
bool serial_open(SerialPort *sp, const char *port_name, DWORD baud, char parity, uint8_t data_bits);
void serial_close(SerialPort *sp);
bool serial_is_open(const SerialPort *sp);

/* Returns false only on a hard I/O error; a timeout with zero bytes read
 * is not an error - out_len is set to 0 and true is returned, mirroring
 * pyserial's read() returning b"" on timeout. */
bool serial_write(SerialPort *sp, const uint8_t *data, DWORD len, DWORD *out_written);
bool serial_read(SerialPort *sp, uint8_t *buf, DWORD buf_size, DWORD *out_len);

/* Enumerates open COM ports via the registry (HARDWARE\DEVICEMAP\SERIALCOMM),
 * writing up to max_ports names (e.g. "COM5") into names[i] (16 bytes each,
 * plenty for "COM" + up to 12 digits). Returns the number of ports found,
 * which may exceed max_ports; callers should treat a returned count >
 * max_ports as "more were found than fit". */
int serial_list_ports(char names[][16], int max_ports);
