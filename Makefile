CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11
LDLIBS := -lasound -lm -lpthread

BIN := synth_engine
SRC := src/backend.c

.PHONY: all run-backend run-frontend clean

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

run-backend: $(BIN)
	./$(BIN)

run-frontend:
	python3 src/frontend.py

clean:
	rm -f $(BIN)
