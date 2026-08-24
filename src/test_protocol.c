/* Mirrors sdr_controller/protocol/test_protocol.py assertions, in C. */
#include "protocol.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

static void print_hex(const uint8_t *data, uint8_t len) {
    uint8_t i;
    for (i = 0; i < len; i++) {
        printf("%02X%s", data[i], (i + 1 < len) ? " " : "");
    }
}

static void test_output_switch(void) {
    ProtoFrame f;
    uint8_t expected[] = { 0x7E, 0x7E, 0x01, 0x05, 0x01, 0x01, 0x0A, 0x0D };
    ProtoStatus st = proto_build_output_switch(&f, 5, true);
    assert(st == PROTO_OK);
    assert(f.len == sizeof(expected));
    assert(memcmp(f.data, expected, sizeof(expected)) == 0);
    printf("output_switch OK: ");
    print_hex(f.data, f.len);
    printf("\n");
}

static void test_signal_control(void) {
    ProtoFrame f;
    uint8_t expected[] = { 0x7E, 0x7E, 0x02, 0x00, 0x05, 0x00, 0x09, 0x92, 0x03, 0x01, 0x0A, 0x0D };
    ProtoStatus st = proto_build_signal_control(&f, 0, PROTO_MODE_WHITE_NOISE, 2450, 100, -6);
    assert(st == PROTO_OK);
    assert(f.len == sizeof(expected));
    assert(memcmp(f.data, expected, sizeof(expected)) == 0);
    printf("signal_control OK: ");
    print_hex(f.data, f.len);
    printf("\n");
    printf("  (note: BufLen here is 0x05, the *correct* byte count - vendor doc says 0x04)\n");
}

static void test_status_query(void) {
    ProtoFrame f;
    uint8_t expected[] = { 0x7E, 0x7E, 0xFF, 0x00, 0x00, 0x0A, 0x0D };
    ProtoStatus st = proto_build_status_query(&f, 0x00);
    assert(st == PROTO_OK);
    assert(f.len == sizeof(expected));
    assert(memcmp(f.data, expected, sizeof(expected)) == 0);
    printf("status_query OK (matches vendor's literal fixed example): ");
    print_hex(f.data, f.len);
    printf("\n");
}

static void test_status_query_with_explicit_address(void) {
    ProtoFrame f;
    uint8_t expected[] = { 0x7E, 0x7E, 0xFF, 0x07, 0x00, 0x0A, 0x0D };
    ProtoStatus st = proto_build_status_query(&f, 7);
    assert(st == PROTO_OK);
    assert(f.len == sizeof(expected));
    assert(memcmp(f.data, expected, sizeof(expected)) == 0);
    printf("status_query_with_explicit_address OK: ");
    print_hex(f.data, f.len);
    printf("\n");
}

static int g_frame_count;
static ProtoParsedFrame g_last_frame;

static void on_frame(const ProtoParsedFrame *frame, void *ctx) {
    (void)ctx;
    g_frame_count++;
    g_last_frame = *frame;
}

static void test_parse_roundtrip(void) {
    ProtoParser parser;
    uint8_t resp[] = { 0x7E, 0x7E, 0xFF, 0x00, 0x06,
                        0x01, 0x00, 0x09, 0x92, 0x03, 0x01,
                        0x0A, 0x0D };
    char desc[128];

    proto_parser_init(&parser);
    g_frame_count = 0;

    proto_parser_feed(&parser, resp, 4, on_frame, NULL);
    assert(g_frame_count == 0);

    proto_parser_feed(&parser, resp + 4, (uint16_t)(sizeof(resp) - 4), on_frame, NULL);
    assert(g_frame_count == 1);
    assert(g_last_frame.type == 0xFF);

    proto_describe(&g_last_frame, desc, sizeof(desc));
    printf("parse_roundtrip OK: %s\n", desc);
}

static void test_parse_with_junk_prefix(void) {
    ProtoParser parser;
    ProtoFrame query;
    uint8_t noisy[2 + PROTO_MAX_FRAME];
    uint16_t noisy_len;

    proto_build_addr_query(&query);
    noisy[0] = 0x00;
    noisy[1] = 0xFF;
    memcpy(noisy + 2, query.data, query.len);
    noisy_len = (uint16_t)(2 + query.len);

    proto_parser_init(&parser);
    g_frame_count = 0;
    proto_parser_feed(&parser, noisy, noisy_len, on_frame, NULL);

    assert(g_frame_count == 1);
    assert(g_last_frame.type == PROTO_TYPE_ADDR_QUERY);
    printf("parse_with_junk_prefix OK\n");
}

int main(void) {
    test_output_switch();
    test_signal_control();
    test_status_query();
    test_status_query_with_explicit_address();
    test_parse_roundtrip();
    test_parse_with_junk_prefix();
    printf("\nAll protocol tests passed.\n");
    return 0;
}
