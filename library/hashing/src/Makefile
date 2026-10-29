# Builds the hashing plugin for the system this is run on.
# The result is put next to library.json.

CC = gcc
CFLAGS = -std=c99 -Wall -Wextra -O2 -I../../../sdk

ifeq ($(OS),Windows_NT)
TARGET = ../hashing.dll
else
TARGET = ../hashing.so
CFLAGS += -fPIC
endif

all: $(TARGET)

$(TARGET): hashing.c
	$(CC) $(CFLAGS) -shared hashing.c -o $(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all clean
