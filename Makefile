# sterm - builds natively on POSIX and cross/native on Windows (MinGW-w64).
#
#   make                       POSIX build
#   make CROSS=x86_64-w64-mingw32-   cross build sterm.exe from Linux
#   (in an MSYS2 MINGW64 shell, plain `make` produces a native sterm.exe)

CROSS   ?=
CC      ?= gcc
# CROSS wins over an inherited CC (make's default CC=cc would otherwise stick)
ifneq ($(CROSS),)
  CC := $(CROSS)gcc
endif
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
PREFIX  ?= /usr/local

# Detect a Windows target from the compiler triplet or the host.
ifneq (,$(findstring mingw,$(CC)))
  WINDOWS = 1
endif
ifeq ($(OS),Windows_NT)
  WINDOWS = 1
endif

ifdef WINDOWS
  BIN   = sterm.exe
  OBJ   = sterm.o plat_win32.o
  LIBS  = -lsetupapi
  # static link so the .exe runs in a bare cmd.exe with no MinGW DLLs
  LDFLAGS += -static
else
  BIN   = sterm
  OBJ   = sterm.o plat_posix.o custom_baud.o
  LIBS  =
endif

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LIBS)

%.o: %.c plat.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)

clean:
	rm -f sterm sterm.exe *.o

.PHONY: all install clean
