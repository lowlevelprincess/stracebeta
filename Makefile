CC := gcc
CFLAGS := -Wall -Wextra -O2 -std=gnu11
SRC := src/mini_strace.c
BIN := mini-strace

.PHONY: all clean run

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $(BIN) $(SRC)

run: $(BIN)
	./$(BIN) /bin/echo hello world

clean:
	rm -f $(BIN)
