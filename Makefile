CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -pthread
TARGET = process_scheduler
SOURCES = process_scheduler.c

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCES)

clean:
	rm -f $(TARGET)

.PHONY: clean

