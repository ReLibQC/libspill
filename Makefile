# libscratch -- POSIX backend, no dependencies beyond pthreads.

CC      ?= cc
CXX     ?= c++
CFLAGS  ?= -O2 -g
CXXFLAGS ?= -O2 -g
CFLAGS  += -std=c99 -D_GNU_SOURCE -Wall -Wextra -pedantic -Iinclude
CXXFLAGS += -std=c++17 -Wall -Wextra -Iinclude -Iport/psi4
DEPFLAGS = -MMD -MP
LDLIBS  += -lpthread

SRC  := src/error.c src/toc.c src/alloc.c src/store.c src/open.c src/async.c
OBJ  := $(SRC:.c=.o)
LIB  := libscratch.a

TESTS := tests/abi_header_test tests/test_posix tests/churn_libscratch
BENCH := bench/ooc_bench

# Psi4's libpsio reimplemented on libscratch. Built here so the port is tested
# without a Psi4 tree; in Psi4 it replaces 23 files and no call site.
PORT_SRC  := port/psi4/psio_libscratch.cc
PORT_TEST := port/psi4/test_psio_shim
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

$(PORT_TEST): port/psi4/test_psio_shim.cc $(PORT_SRC) $(LIB)
	$(CXX) $(CXXFLAGS) -o $@ port/psi4/test_psio_shim.cc $(PORT_SRC) $(LIB) $(LDLIBS)

check: $(TESTS) $(PORT_TEST)
	@for t in $(TESTS) $(PORT_TEST); do echo "== $$t"; ./$$t || exit 1; done

bench: $(BENCH)

clean:
	rm -f $(OBJ) $(DEP) $(LIB) $(TESTS) $(BENCH) $(PORT_TEST)

-include $(DEP)

.PHONY: all check bench clean
