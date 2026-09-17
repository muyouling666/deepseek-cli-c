# Makefile - DeepSeek CLI client (C + libcurl + cJSON)
#
# Quick start:
#   make                 # build ./ai
#   make deps            # build cJSON + libcurl inside ./build (no system packages)
#   make clean
#
# The client needs libcurl. Two supported setups:
#
#   1. libcurl is already installed (Linux: libcurl4-openssl-dev,
#      macOS: brew install curl, MSYS2: pacman -S mingw-w64-x86_64-curl).
#      Then "make" just works.
#
#   2. Nothing is installed: run "make deps" first. It compiles the vendored
#      cJSON and builds libcurl from source into ./build, then writes
#      build/deps.mk, which this Makefile picks up automatically.
#
# Manual overrides:
#   make LIBCURL_CFLAGS=-I/opt/curl/include LIBCURL_LIBS="-L/opt/curl/lib -lcurl"
#
# On Windows, scripts/Setup-Build.ps1 writes build/local.mk with the compiler
# and library paths; it is included below as well.

TARGET  := ai
# MinGW and MSVC need the .exe suffix; everywhere else the binary is plain "ai"
# so that "./ai \"你好\"" works as documented.
ifeq ($(OS),Windows_NT)
  EXEEXT := .exe
else
  EXEEXT :=
endif
CC      ?= cc
SRCDIR  := src
BUILD   := build

# ------------------------------------------------------------------ #
# Sources                                                             #
# ------------------------------------------------------------------ #

CLIENT_SRCS := $(SRCDIR)/main.c $(SRCDIR)/http.c $(SRCDIR)/conversation.c $(SRCDIR)/util.c $(SRCDIR)/console.c
CLIENT_OBJS := $(patsubst $(SRCDIR)/%.c,$(BUILD)/%.o,$(CLIENT_SRCS))
DEPS        := $(CLIENT_OBJS:.o=.d)

# cJSON: vendored copy by default, otherwise use the system one.
CJSON_SRC := $(wildcard vendor/cJSON.c)
ifeq ($(CJSON_SRC),)
  CJSON_OBJ  :=
  CJSON_OBJS := $(shell pkg-config --libs cjson 2>/dev/null)
  CJSON_CFLAGS :=
else
  CJSON_OBJ  := $(BUILD)/cJSON.o
  CJSON_OBJS :=
  CJSON_CFLAGS := -Ivendor
endif

# Objects that need to be linked into the binary.
OBJS := $(CLIENT_OBJS) $(CJSON_OBJ)

# ------------------------------------------------------------------ #
# Flags                                                               #
# ------------------------------------------------------------------ #

# build/deps.mk (written by "make deps") and build/local.mk (written by
# scripts/Setup-Build.ps1) override these when present, so include them first.
-include $(BUILD)/deps.mk
-include $(BUILD)/local.mk

LIBCURL_CFLAGS ?=
LIBCURL_LIBS   ?= -lcurl

WARN     := -Wall -Wextra -Wshadow -Wpointer-arith -Wwrite-strings -Wstrict-prototypes
CPPFLAGS += -I$(SRCDIR) $(CJSON_CFLAGS) $(LIBCURL_CFLAGS) -D_POSIX_C_SOURCE=200809L
CFLAGS   ?= -O2
CFLAGS   += -std=c99 $(WARN)
LDLIBS   += $(LIBCURL_LIBS) $(CJSON_OBJS)

# A static libcurl needs CURL_STATICLIB so curl.h does not mark the whole API
# as dllimport. "make deps" and scripts/Setup-Build.ps1 enable it through the
# generated include file; STATIC_CURL=1 does the same by hand, e.g.
#   make STATIC_CURL=1 LIBCURL_LIBS="/usr/lib/libcurl.a -lssl -lcrypto -lz"
ifdef STATIC_CURL
  CPPFLAGS += -DCURL_STATICLIB
endif

ifeq ($(OS),Windows_NT)
  LDLIBS += -lws2_32 -lbcrypt -lsecur32 -lcrypt32 -lshell32
  # Avoid a runtime dependency on libgcc_s_*.dll / libwinpthread-1.dll so the
  # produced exe can be copied anywhere.
  LDFLAGS += -static-libgcc
  ifneq (,$(findstring posix,$(shell "$(CC)" -dumpmachine 2>/dev/null)))
    LDFLAGS += -static-libwinpthread
  endif
  # "clean" runs through cmd.exe, which has no rm. Override with
  # "make RM=rm clean" under MSYS2/Cygwin.
  RM ?= cmd /c "if exist $(BUILD) rmdir /s /q $(BUILD)" && cmd /c "del /q $(TARGET)$(EXEEXT) 2>nul"
endif

# Drop this in and the whole thing is one relocatable binary.
ifdef STATIC
  LDFLAGS += -static
endif

RM ?= rm -rf

# ------------------------------------------------------------------ #
# Targets                                                             #
# ------------------------------------------------------------------ #

.PHONY: all clean deps setup test info

all: $(TARGET)$(EXEEXT)

$(TARGET)$(EXEEXT): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(BUILD)/%.o: $(SRCDIR)/%.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/cJSON.o: vendor/cJSON.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD):
	@mkdir -p $(BUILD)

# Build the dependencies in-tree (see scripts/fetch-deps.sh for details).
# Needs a POSIX shell; on Windows without one use "make setup" instead.
deps:
	@sh scripts/fetch-deps.sh

# Windows without a POSIX shell: same result via PowerShell.
setup:
	@powershell -NoProfile -ExecutionPolicy Bypass -File scripts/Setup-Build.ps1 $(SETUP_ARGS)

# End-to-end tests against tests/mock_server.py (no API key needed).
test: all
	@python tests/run_tests.py

info:
	@echo "CC             : $(CC)"
	@echo "CFLAGS         : $(CFLAGS)"
	@echo "CPPFLAGS       : $(CPPFLAGS)"
	@echo "LIBCURL_CFLAGS : $(LIBCURL_CFLAGS)"
	@echo "LIBCURL_LIBS   : $(LIBCURL_LIBS)"
	@echo "CJSON_OBJ      : $(CJSON_OBJ)"
	@echo "LDLIBS         : $(LDLIBS)"

clean:
	$(RM) $(BUILD) $(TARGET) $(TARGET).exe

-include $(DEPS)
