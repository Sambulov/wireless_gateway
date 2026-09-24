/* WsFrameParser test list (delete each line as its TEST is written):
 * [x] NULL payload → WS_PARSE_NO_PAYLOAD
 * [x] Zero-length payload → WS_PARSE_NO_PAYLOAD
 * [x] Malformed JSON → WS_PARSE_BAD_JSON
 * [x] FID missing (not in JSON) → WS_PARSE_BAD_FID (parsed as 0)
 * [x] FID = 0 explicitly → WS_PARSE_BAD_FID
 * [x] FID as decimal integer → WS_PARSE_OK, correct fid
 * [x] FID as hex string "0x1012" → WS_PARSE_OK, correct fid
 * [x] FLAGS field missing → WS_PARSE_MISSING_FLAGS
 * [x] SID field missing → WS_PARSE_MISSING_SID
 * [x] SID masked to lower 16 bits (0x10001 → 1)
 * [x] FLAGS value forwarded verbatim
 */
extern "C" {
#include "ws_frame_parser.h"
}
#include "CppUTest/TestHarness.h"
#include <string.h>

TEST_GROUP(WsFrameParser)
{
    ws_parsed_frame_t frame;

    void setup()
    {
        memset(&frame, 0xaa, sizeof(frame));
    }
    void teardown() {}
};

/* --- NULL / empty payload --- */

TEST(WsFrameParser, NullPayloadReturnsNoPayload)
{
    // Exercise + Verify
    LONGS_EQUAL(WS_PARSE_NO_PAYLOAD, WsFrame_Parse(NULL, 0, &frame));
}

TEST(WsFrameParser, ZeroLengthPayloadReturnsNoPayload)
{
    LONGS_EQUAL(WS_PARSE_NO_PAYLOAD, WsFrame_Parse("", 0, &frame));
}

/* --- JSON validity --- */

TEST(WsFrameParser, MalformedJsonReturnsBadJson)
{
    const char *bad = "{not valid json}";
    LONGS_EQUAL(WS_PARSE_BAD_JSON, WsFrame_Parse(bad, strlen(bad), &frame));
}

/* --- FID validation --- */

TEST(WsFrameParser, MissingFidReturnsBadFid)
{
    const char *msg = "{\"FLAGS\":0,\"SID\":1}";
    LONGS_EQUAL(WS_PARSE_BAD_FID, WsFrame_Parse(msg, strlen(msg), &frame));
}

TEST(WsFrameParser, ZeroFidReturnsBadFid)
{
    const char *msg = "{\"FID\":0,\"FLAGS\":0,\"SID\":1}";
    LONGS_EQUAL(WS_PARSE_BAD_FID, WsFrame_Parse(msg, strlen(msg), &frame));
}

TEST(WsFrameParser, FidAsDecimalInteger)
{
    const char *msg = "{\"FID\":1000,\"FLAGS\":0,\"SID\":5}";
    ws_parse_result_t result = WsFrame_Parse(msg, strlen(msg), &frame);
    LONGS_EQUAL(WS_PARSE_OK, result);
    LONGS_EQUAL(1000, (long)frame.fid);
}

TEST(WsFrameParser, FidAsHexString)
{
    const char *msg = "{\"FID\":\"0x1012\",\"FLAGS\":0,\"SID\":5}";
    ws_parse_result_t result = WsFrame_Parse(msg, strlen(msg), &frame);
    LONGS_EQUAL(WS_PARSE_OK, result);
    LONGS_EQUAL(0x1012, (long)frame.fid);
}

/* --- FLAGS / SID mandatory fields --- */

TEST(WsFrameParser, MissingFlagsReturnsMissingFlags)
{
    const char *msg = "{\"FID\":1000,\"SID\":5}";
    LONGS_EQUAL(WS_PARSE_MISSING_FLAGS, WsFrame_Parse(msg, strlen(msg), &frame));
}

TEST(WsFrameParser, MissingSidReturnsMissingSid)
{
    const char *msg = "{\"FID\":1000,\"FLAGS\":0}";
    LONGS_EQUAL(WS_PARSE_MISSING_SID, WsFrame_Parse(msg, strlen(msg), &frame));
}

/* --- Output field correctness --- */

TEST(WsFrameParser, SidIsMaskedToLower16Bits)
{
    /* 0x10001 & 0xffff == 1 — matching ws_server.c line 405 */
    const char *msg = "{\"FID\":1000,\"FLAGS\":2,\"SID\":65537}";
    ws_parse_result_t result = WsFrame_Parse(msg, strlen(msg), &frame);
    LONGS_EQUAL(WS_PARSE_OK, result);
    LONGS_EQUAL(1, (long)frame.sid);
    LONGS_EQUAL(2, (long)frame.flags);
}

TEST(WsFrameParser, FlagsValueIsForwardedVerbatim)
{
    const char *msg = "{\"FID\":2000,\"FLAGS\":7,\"SID\":42}";
    ws_parse_result_t result = WsFrame_Parse(msg, strlen(msg), &frame);
    LONGS_EQUAL(WS_PARSE_OK, result);
    LONGS_EQUAL(7, (long)frame.flags);
    LONGS_EQUAL(42, (long)frame.sid);
}
