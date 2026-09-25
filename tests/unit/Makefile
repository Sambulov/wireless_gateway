ROOT     := ../..
CPPUTEST := $(ROOT)/tests/cpputest/build_host/install
BUILD    := build

# ── Compiler flags ────────────────────────────────────────────────────────
INCLUDES := \
    -Istubs \
    -I$(ROOT)/ws_server/include \
    -I$(ROOT)/CodeLibrary/CL \
    -I$(ROOT)/json/cJSON \
    -I$(ROOT)/json \
    -I$(ROOT)/main \
    -I$(CPPUTEST)/include

# ASan/UBSan: regression tests for memory bugs (use-after-free, NULL
# writes) only fail reliably under a sanitizer. UBSan is made fatal —
# by default it only prints and the run still reports OK.
SANITIZE := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer

CFLAGS   := -Wall -Wextra -g -DWS_SERVER_TEST -DAPIH_UART_TEST $(SANITIZE) $(INCLUDES)
CXXFLAGS := -Wall -Wextra -g -DWS_SERVER_TEST -DAPIH_UART_TEST $(SANITIZE) $(INCLUDES)
LDFLAGS  := -L$(CPPUTEST)/lib -lCppUTest -lCppUTestExt $(SANITIZE)

# ── Source → object mapping ───────────────────────────────────────────────
# VPATH lets Make find sources by basename without per-rule path repetition.
VPATH := \
    $(ROOT)/ws_server/src \
    $(ROOT)/main \
    $(ROOT)/json/cJSON \
    $(ROOT)/json \
    $(ROOT)/CodeLibrary/CL/DataStructures/Src \
    $(ROOT)/CodeLibrary/CL/Converters/Src \
    stubs

C_SRCS := \
    ws_server.c \
    ws_frame_parser.c \
    apih_uart.c \
    cJSON.c \
    LinkedList.c \
    Mem.c \
    Str.c \
    Base64Encoding.c \
    cJSON_helpers.c \
    ws_hal_stub.c \
    ws_conn_stub.c \
    gw_uart_stub.c

CPP_SRCS := \
    AllTests.cpp \
    WsFrameParserTest.cpp \
    WsServerTest.cpp \
    ApihUartTest.cpp

C_OBJS   := $(patsubst %.c,  $(BUILD)/%.o, $(C_SRCS))
CPP_OBJS := $(patsubst %.cpp,$(BUILD)/%.o, $(CPP_SRCS))
OBJS     := $(C_OBJS) $(CPP_OBJS)

TARGET := $(BUILD)/test_runner

# ── Targets ───────────────────────────────────────────────────────────────
.PHONY: all run clean

all: $(TARGET)

run: $(TARGET)
	$< -v

# Rebuild everything when flags change here (e.g. objects left from a build
# without sanitizers would otherwise link silently uninstrumented).
$(OBJS): Makefile

$(TARGET): $(OBJS) | $(BUILD)
	g++ $^ $(LDFLAGS) -o $@

$(BUILD)/%.o: %.c | $(BUILD)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.cpp | $(BUILD)
	g++ $(CXXFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)
