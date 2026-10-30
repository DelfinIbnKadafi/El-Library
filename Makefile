CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -O2
ifeq ($(OS),Windows_NT)
	TARGET = ellib.exe
else
    TARGET = ellib
endif
SOURCE = ellib.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) $(SOURCE) -o $(TARGET)

clean:
	rm -f $(TARGET)