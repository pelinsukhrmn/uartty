CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
PREFIX  ?= /usr/local

OBJ = sterm.o custom_baud.o

all: sterm

sterm: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

install: sterm
	install -Dm755 sterm $(DESTDIR)$(PREFIX)/bin/sterm

clean:
	rm -f sterm $(OBJ)

.PHONY: all install clean
