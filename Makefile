CC       ?= cc
CFLAGS   ?= -O2 -g
CFLAGS   += -std=c11 -Wall -Wextra -Wpedantic
CPPFLAGS += -Isrc
LDFLAGS  ?=
LDLIBS   ?=

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
BIN := miniredis

.PHONY: all clean test test-unit test-integration

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

# Unit tests (no networking required).
tests/test_util: tests/test_util.c src/util.c src/util.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_util.c src/util.c

tests/test_dict: tests/test_dict.c src/dict.c src/dict.h src/util.c src/util.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_dict.c src/dict.c src/util.c

tests/test_resp: tests/test_resp.c src/resp.c src/resp.h src/util.c src/util.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_resp.c src/resp.c src/util.c

# End-to-end client used by tests/integration.sh.
tests/test_client: tests/test_client.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_client.c

test-unit: tests/test_util tests/test_dict tests/test_resp
	./tests/test_util
	./tests/test_dict
	./tests/test_resp

test-integration: $(BIN) tests/test_client
	sh ./tests/integration.sh

test: test-unit test-integration

clean:
	rm -f $(BIN) $(OBJ) tests/test_util tests/test_dict tests/test_resp tests/test_client
