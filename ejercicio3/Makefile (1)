CC=gcc
CFLAGS=-Wall -Wextra -pedantic -std=gnu99
TARGET=main

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $(TARGET) main.c

clean:
	rm -f $(TARGET) *.o procesados.txt salida.txt

.PHONY: all clean
