# Builds the mysql plugin for the system this is run on.
# The result is put next to library.json.
#
# elplugin.h (the plugin API header) is copied next to mysql.c, so this folder
# builds on its own.
#
# The MySQL client library is NOT needed to build, the plugin loads it when a
# program connects. Only a C compiler is needed.

CC = gcc
CFLAGS = -std=c99 -Wall -Wextra -O2 -I.

ifeq ($(OS),Windows_NT)
TARGET = ../mysql.dll
LDLIBS =
else
UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
TARGET = ../mysql.dylib
SHARED = -dynamiclib
LDLIBS =
else
TARGET = ../mysql.so
LDLIBS = -ldl
endif
CFLAGS += -fPIC
endif

SHARED ?= -shared

all: $(TARGET)

$(TARGET): mysql.c
	$(CC) $(CFLAGS) $(SHARED) mysql.c -o $(TARGET) $(LDLIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
