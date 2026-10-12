CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -O2
TARGET = ellib.exe
SOURCE = ellib.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) $(SOURCE) -o $(TARGET)

clean:
	rm -f $(TARGET)