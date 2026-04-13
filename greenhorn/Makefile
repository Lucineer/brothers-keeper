CC = gcc
CFLAGS = -std=gnu99 -Wall -O2 -Os
TARGET = vm

all: $(TARGET) test_fib

$(TARGET): test_fib.c vm.c vm.h
	$(CC) $(CFLAGS) -o $@ test_fib.c vm.c

$(TARGET).static: test_fib.c vm.c vm.h
	$(CC) $(CFLAGS) -static -o $@ test_fib.c vm.c

test_fib: test_fib.c vm.c vm.h
	$(CC) $(CFLAGS) -o $@ test_fib.c vm.c

clean:
	rm -f $(TARGET) test_fib

size: $(TARGET)
	size $(TARGET)

.PHONY: all clean size
