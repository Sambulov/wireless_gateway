ROOT     := ../..
CPPUTEST := $(ROOT)/tests/cpputest/build_host/install
BUILD    := build

# ── Compiler flags ────────────────────────────────────────────────────────
INCLUDES := \
    -Istubs \
    -I$(ROOT)/api/include \
    -I$(ROOT)/transport/include \
    -I$(ROOT)/platform/include \
    -I$(ROOT)/CodeLibrary/CL \
    -I$(ROOT)/json/jsmn \
    -I$(ROOT)/json \
    -I$(ROOT)/main \
    -I$(CPPUTEST)/include

# ASan/UBSan: regression tests for memory bugs (use-after-free, NULL
# writes) only fail reliably under a sanitizer. UBSan is made fatal —
# by default it only prints and the run still reports OK.
SANITIZE := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer

# APIH_UART_TEST / APIH_HOST_TEST — точки входа для тестов: службы отдают под ними
# свой сброс состояния, а apih_sys_internal.h подключает тень app.h вместо
# прошивочного (см. apih_uart.c, apih_fw.c).
CFLAGS   := -Wall -Wextra -g -DAPIH_UART_TEST -DAPIH_HOST_TEST $(SANITIZE) $(INCLUDES)
CXXFLAGS := -Wall -Wextra -g -DAPIH_UART_TEST -DAPIH_HOST_TEST $(SANITIZE) $(INCLUDES)
LDFLAGS  := -L$(CPPUTEST)/lib -lCppUTest -lCppUTestExt $(SANITIZE)

# ── Source → object mapping ───────────────────────────────────────────────
# VPATH lets Make find sources by basename without per-rule path repetition.
# Компонент transport — только заголовки (transport.h): исходников у него нет, в
# VPATH ему делать нечего, хватает -I выше.
VPATH := \
    $(ROOT)/api/src \
    $(ROOT)/main \
    $(ROOT)/json \
    $(ROOT)/json/jsmn \
    $(ROOT)/CodeLibrary/CL/DataStructures/Src \
    $(ROOT)/CodeLibrary/CL/Converters/Src \
    $(ROOT)/CodeLibrary/CL/Workflow/Src \
    stubs

C_SRCS := \
    api_core.c \
    apih_uart.c \
    apih_fw.c \
    apih_sys_common.c \
    web_iface_policy.c \
    fs_path.c \
    uart_rxbuf.c \
    GwJson.c \
    jsmn.c \
    LinkedList.c \
    Event.c \
    Mem.c \
    Str.c \
    Base64Encoding.c \
    system_stub.c \
    transport_stub.c \
    gw_uart_stub.c \
    net_stub.c \
    esp_ota_stub.c \
    esp_rom_md5_stub.c

CPP_SRCS := \
    AllTests.cpp \
    GwJsonTest.cpp \
    WsServerTest.cpp \
    ApihUartTest.cpp \
    ApihFwTest.cpp \
    WebIfacePolicyTest.cpp \
    FsPathTest.cpp \
    UartRxbufTest.cpp

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

# -MMD: рядом с объектом кладётся список заголовков, от которых он зависит.
# Без этого правка uart.h/app.h не пересобирала объекты, и тесты падали на
# старых объектах со старой раскладкой структур (проверено: три теста «падали»
# из-за устаревшего gw_uart_stub.o после добавления поля в gw_uart_config_t).
$(BUILD)/%.o: %.c | $(BUILD)
	gcc $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.cpp | $(BUILD)
	g++ $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(C_OBJS:.o=.d) $(CPP_OBJS:.o=.d)

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)
