
CC = gcc
CFLAGS = -std=c99 -Wall -Wextra -O2
INCLUDE = -I C:\Projects\C\EL-Library\sdk

ifeq ($(OS),Windows_NT)
    TARGET = dotenv.dll
else
    TARGET = dotenv.so
    CFLAGS += -fPIC
    INCLUDE = -I /mnt/c/Projects/C/EL-Library/sdk
endif

all: $(TARGET)

$(TARGET): src/dotenv.c
	$(CC) $(CFLAGS) $(INCLUDE) -shared $< -o $@

clean:
	rm -f $(TARGET)

.PHONY: all clean
