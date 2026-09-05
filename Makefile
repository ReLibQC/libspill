# libscratch -- POSIX backend, no dependencies beyond pthreads.

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c99 -D_GNU_SOURCE -Wall -Wextra -pedantic -Iinclude
DEPFLAGS = -MMD -MP
LDLIBS  += -lpthread

SRC  := src/error.c src/toc.c src/alloc.c src/store.c src/open.c src/async.c
OBJ  := $(SRC:.c=.o)
LIB  := libscratch.a

TESTS := tests/abi_header_test tests/test_posix tests/churn_libscratch
BENCH := bench/ooc_bench
DEP   := $(OBJ:.o=.d) $(TESTS:=.d) $(BENCH:=.d)

all: $(LIB)

$(LIB): $(OBJ)
	$(AR) rcs $@ $(OBJ)

%.o: %.c
	$(CC) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

tests/%: tests/%.c $(LIB)
	$(CC) $(CFLAGS) $(DEPFLAGS) -MF $@.d -o $@ $< $(LIB) $(LDLIBS)

bench/%: bench/%.c $(LIB)
	$(CC) $(CFLAGS) $(DEPFLAGS) -MF $@.d -o $@ $< $(LIB) $(LDLIBS)

check: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; done

bench: $(BENCH)

clean:
	rm -f $(OBJ) $(DEP) $(LIB) $(TESTS) $(BENCH)

-include $(DEP)

.PHONY: all check bench clean
