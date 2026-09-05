# libscratch -- POSIX backend, no dependencies beyond pthreads.

CC      ?= cc
CXX     ?= c++
# make has a built-in default of f77 for FC, so ?= never fires. Override only
# when the value is that built-in, leaving a user's FC=... alone.
ifeq ($(origin FC),default)
FC      := gfortran
endif
CFLAGS  ?= -O2 -g
CXXFLAGS ?= -O2 -g
CFLAGS  += -std=c99 -D_GNU_SOURCE -Wall -Wextra -pedantic -Iinclude
CXXFLAGS += -std=c++17 -Wall -Wextra -Iinclude -Iport/psi4
FCFLAGS  ?= -O2 -g
FCFLAGS  += -std=f2008 -Wall -Jfortran -Ifortran
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

# OpenMolcas's DaFile family, over the Fortran binding of §4a. Needs a Fortran
# compiler; `make check-c` skips it.
FORT_OBJ  := fortran/libscratch.o port/openmolcas/dafile_libscratch.o
FORT_TEST := port/openmolcas/test_dafile_shim
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

fortran/libscratch.o: fortran/libscratch.F90 include/libscratch.h
	$(FC) $(FCFLAGS) -c -o $@ $<

port/openmolcas/dafile_libscratch.o: port/openmolcas/dafile_libscratch.F90 fortran/libscratch.o
	$(FC) $(FCFLAGS) -c -o $@ $<

$(FORT_TEST): port/openmolcas/test_dafile_shim.F90 $(FORT_OBJ) $(LIB)
	$(FC) $(FCFLAGS) -o $@ $< $(FORT_OBJ) $(LIB) $(LDLIBS)

check-c: $(TESTS) $(PORT_TEST)
	@for t in $(TESTS) $(PORT_TEST); do echo "== $$t"; ./$$t || exit 1; done

check: check-c $(FORT_TEST)
	@echo "== $(FORT_TEST)"; ./$(FORT_TEST)

bench: $(BENCH)

clean:
	rm -f $(OBJ) $(DEP) $(LIB) $(TESTS) $(BENCH) $(PORT_TEST) \
	      $(FORT_OBJ) $(FORT_TEST) fortran/*.mod

-include $(DEP)

.PHONY: all check check-c bench clean
