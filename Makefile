# libspill -- POSIX backend, no dependencies beyond pthreads.

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
LIB  := libspill.a

TESTS := tests/abi_header_test tests/test_posix tests/churn_libspill \
         tests/test_surveyed tests/test_crayio
BENCH := bench/ooc_bench

# Psi4's libpsio reimplemented on libspill. Built here so the port is tested
# without a Psi4 tree; in Psi4 it replaces 23 files and no call site.
PORT_SRC  := port/psi4/psio_libspill.cc
PORT_TEST := port/psi4/test_psio_shim

# Conformance against Psi4's OWN headers rather than our copy of its types.
# Skipped when no Psi4 tree is present; point PSI4_DIR at one to run it.
PSI4_DIR  ?= /home/work/psi4/psi4

# OpenMolcas's DaFile family, over the Fortran binding of §4a. Needs a Fortran
# compiler; `make check-c` skips it.
FORT_OBJ  := fortran/libspill.o port/openmolcas/molcas_kinds.o \
             port/openmolcas/molcas_stubs.o \
             port/openmolcas/dafile_libspill.o port/openmolcas/runfile_libspill.o
FORT_TEST := port/openmolcas/test_dafile_shim port/openmolcas/test_runfile_shim
DEP   := $(OBJ:.o=.d) $(TESTS:=.d) $(BENCH:=.d)

all: $(LIB)

$(LIB): $(OBJ)
	$(AR) rcs $@ $(OBJ)

%.o: %.c
	$(CC) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

tests/%: tests/%.c $(LIB)
	$(CC) $(CFLAGS) $(DEPFLAGS) -MF $@.d -o $@ $< $(LIB) $(LDLIBS)

# The crayio conformance target (§6a) is a shim plus its test, not one file.
tests/test_crayio: tests/test_crayio.c tests/crayio_shim.c tests/crayio_shim.h $(LIB)
	$(CC) $(CFLAGS) -Itests -o $@ tests/test_crayio.c tests/crayio_shim.c $(LIB) $(LDLIBS)

bench/%: bench/%.c $(LIB)
	$(CC) $(CFLAGS) $(DEPFLAGS) -MF $@.d -o $@ $< $(LIB) $(LDLIBS)

$(PORT_TEST): port/psi4/test_psio_shim.cc $(PORT_SRC) $(LIB)
	$(CXX) $(CXXFLAGS) -o $@ port/psi4/test_psio_shim.cc $(PORT_SRC) $(LIB) $(LDLIBS)

fortran/libspill.o: fortran/libspill.F90 include/libspill.h
	$(FC) $(FCFLAGS) -c -o $@ $<

port/openmolcas/molcas_kinds.o: port/openmolcas/molcas_kinds.F90
	$(FC) $(FCFLAGS) -c -o $@ $<

port/openmolcas/%.o: port/openmolcas/%.F90 fortran/libspill.o port/openmolcas/molcas_kinds.o
	$(FC) $(FCFLAGS) -c -o $@ $<

port/openmolcas/test_%_shim: port/openmolcas/test_%_shim.F90 $(FORT_OBJ) $(LIB)
	$(FC) $(FCFLAGS) -o $@ $< $(FORT_OBJ) $(LIB) $(LDLIBS)

port/psi4/conformance_psi4: port/psi4/conformance_psi4.cc $(PORT_SRC) $(LIB)
	$(CXX) $(CXXFLAGS) -DPSIO_USE_PSI4_HEADERS \
	    -I$(PSI4_DIR)/src -I$(PSI4_DIR)/include \
	    -o $@ port/psi4/conformance_psi4.cc $(PORT_SRC) $(LIB) $(LDLIBS)

check-psi4:
	@if [ -f "$(PSI4_DIR)/src/psi4/libpsio/psio.h" ]; then \
	    $(MAKE) --no-print-directory port/psi4/conformance_psi4 && ./port/psi4/conformance_psi4; \
	 else \
	    echo "  skipped: no Psi4 tree at $(PSI4_DIR); set PSI4_DIR=... to run"; \
	 fi

check-c: $(TESTS) $(PORT_TEST)
	@for t in $(TESTS) $(PORT_TEST); do echo "== $$t"; ./$$t || exit 1; done

check: check-c $(FORT_TEST)
	@for t in $(FORT_TEST); do echo "== $$t"; ./$$t || exit 1; done
	@echo "== psi4 header conformance"; $(MAKE) --no-print-directory check-psi4

bench: $(BENCH)

clean:
	rm -f $(OBJ) $(DEP) $(LIB) $(TESTS) $(BENCH) $(PORT_TEST) \
	      $(FORT_OBJ) $(FORT_TEST) fortran/*.mod port/psi4/conformance_psi4

-include $(DEP)

.PHONY: all check check-c check-psi4 bench clean
