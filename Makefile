CC := gcc
CFLAGS := -Wall -Wextra -O2 -std=gnu11
SRC := src/mini_strace.c
BIN := mini-strace
SYSCALL_TABLE := src/syscall_names.h

.PHONY: all clean run $(SYSCALL_TABLE)

all: $(BIN)

# Regenerated every build: the syscall table is architecture-specific
# (x86-64 and ARM64 use completely different numbering), so it always
# needs to match whatever machine you're building on.
$(SYSCALL_TABLE):
	bash scripts/gen_syscall_table.sh > $(SYSCALL_TABLE)

$(BIN): $(SRC) $(SYSCALL_TABLE)
	$(CC) $(CFLAGS) -o $(BIN) $(SRC)

run: $(BIN)
	./$(BIN) /bin/echo hello world

clean:
	rm -f $(BIN) $(SYSCALL_TABLE)
