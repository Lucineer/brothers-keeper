CC = gcc
CFLAGS = -std=gnu99 -Wall -Wextra -O2 -Os

all: flux-asm flux-disasm flux-link

flux-asm: flux-asm.c
	$(CC) $(CFLAGS) -o $@ $<
	strip $@

flux-disasm: flux-disasm.c
	$(CC) $(CFLAGS) -o $@ $<
	strip $@

flux-link: flux-link.c
	$(CC) $(CFLAGS) -o $@ $<
	strip $@

clean:
	rm -f flux-asm flux-disasm flux-link *.o *.bin *.sym

.PHONY: all clean
