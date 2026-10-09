# Makefile do banco chave-valor.
# Uso: make, make test, make clean. CC, CFLAGS e LDFLAGS podem ser trocados:
#   make test CFLAGS="-O1 -g -fsanitize=address,undefined" LDFLAGS="-fsanitize=address,undefined"

CFLAGS  ?= -O2 -g
LDFLAGS ?=
WARN    := -std=c11 -Wall -Wextra -Wpedantic
ALL_CFLAGS = $(WARN) -Iinclude $(CFLAGS)

B := build
HEADERS := $(wildcard include/*.h)
LIB := $(B)/libkv.a
TESTES := $(B)/test_kv $(B)/test_queda

.PHONY: all test clean

all: $(LIB) $(B)/kv

$(B):
	mkdir -p $@

$(B)/%.o: src/%.c $(HEADERS) | $(B)
	$(CC) $(ALL_CFLAGS) -c $< -o $@

$(LIB): $(B)/kv.o
	$(AR) rcs $@ $^

$(B)/kv: $(B)/kv_cli.o $(LIB)
	$(CC) $(ALL_CFLAGS) $^ $(LDFLAGS) -o $@

$(B)/test_%: tests/test_%.c tests/teste.h $(LIB)
	$(CC) $(ALL_CFLAGS) $< $(LIB) $(LDFLAGS) -o $@

test: all $(TESTES)
	$(B)/test_kv
	$(B)/test_queda
	sh tests/cli.sh $(B)

clean:
	rm -rf $(B)
