# SPDX-License-Identifier: BSD-3-Clause
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
# -fvisibility=hidden keeps the ~34 internal ls_* helpers out of a consumer's
# exported namespace; -fPIC lets the static library be vendored INTO someone
# else's shared object, which is how these codes would take it. CMake sets
# POSITION_INDEPENDENT_CODE for the same reason.
CFLAGS  += -fvisibility=hidden -fPIC
CXXFLAGS += -std=c++17 -Wall -Wextra -Iinclude
FCFLAGS  ?= -O2 -g
FCFLAGS  += -std=f2008 -Wall -Jfortran -Ifortran
DEPFLAGS = -MMD -MP
LDLIBS  += -lpthread

SRC  := src/error.c src/toc.c src/alloc.c src/store.c src/open.c src/async.c

# The optional HDF5 backend (§7b). Enabled when the headers are present; a
# library built without them still accepts LS_HDF5 at compile time and refuses
# it at ls_open with LS_ERR_BACKEND.
# Debian and Ubuntu put the serial build under a subdirectory with a suffixed
# library name; everyone else uses the plain paths.
ifneq ($(wildcard /usr/include/hdf5.h /usr/local/include/hdf5.h),)
SRC     += src/hdf5.c
CFLAGS  += -DLIBSPILL_HAVE_HDF5
LDLIBS  += -lhdf5
else ifneq ($(wildcard /usr/include/hdf5/serial/hdf5.h),)
SRC     += src/hdf5.c
CFLAGS  += -DLIBSPILL_HAVE_HDF5 -I/usr/include/hdf5/serial
LDLIBS  += -lhdf5_serial
endif
OBJ  := $(SRC:.c=.o)
LIB  := libspill.a
SO   := libspill.so

TESTS := tests/abi_header_test tests/test_posix tests/churn_libspill \
         tests/test_surveyed tests/test_crayio tests/test_crayio_i8 tests/test_mapped \
         tests/test_hdf5
BENCH := bench/ooc_bench



# The header-only C++ layer of §4a. C++20 for std::span.
CXX_TEST := tests/test_cxx

# The Fortran binding of §4a, and its own test. The port shims that used to
# provide the only Fortran coverage now live in the codes they adapt.
FORT_OBJ  := fortran/libspill.o
FORT_TEST := tests/test_fortran
DEP   := $(OBJ:.o=.d) $(TESTS:=.d) $(BENCH:=.d)

all: $(LIB) $(SO)

$(LIB): $(OBJ)
	$(AR) rcs $@ $(OBJ)

# The Python binding loads the library through ctypes, so it needs a shared
# object. -fPIC is applied to a separate set of objects rather than to the
# static library, which callers link into their own binaries.
$(SO): $(SRC) include/libspill.h src/internal.h
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $(SRC) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

tests/%: tests/%.c $(LIB)
	$(CC) $(CFLAGS) $(DEPFLAGS) -MF $@.d -o $@ $< $(LIB) $(LDLIBS)

# The crayio compatibility layer. Built at BOTH Fortran integer widths, because
# every argument arrives by pointer and a mismatch reads bytes the caller never
# wrote -- and, being Fortran externals, nothing downstream would catch it.
CRAYIO_SRC := compat/crayio/crayio_libspill.c
tests/test_crayio: compat/crayio/test_crayio.c $(CRAYIO_SRC) compat/crayio/crayio_libspill.h $(LIB)
	$(CC) $(CFLAGS) -Icompat/crayio -o $@ compat/crayio/test_crayio.c $(CRAYIO_SRC) $(LIB) $(LDLIBS)

tests/test_crayio_i8: compat/crayio/test_crayio.c $(CRAYIO_SRC) compat/crayio/crayio_libspill.h $(LIB)
	$(CC) $(CFLAGS) -DVAR_INT64 -Icompat/crayio -o $@ compat/crayio/test_crayio.c $(CRAYIO_SRC) $(LIB) $(LDLIBS)

bench/%: bench/%.c $(LIB)
	$(CC) $(CFLAGS) $(DEPFLAGS) -MF $@.d -o $@ $< $(LIB) $(LDLIBS)

fortran/libspill.o: fortran/libspill.F90 include/libspill.h
	$(FC) $(FCFLAGS) -c -o $@ $<

tests/test_fortran: tests/test_fortran.F90 $(FORT_OBJ) $(LIB)
	$(FC) $(FCFLAGS) -o $@ $< $(FORT_OBJ) $(LIB) $(LDLIBS)


$(CXX_TEST): tests/test_cxx.cc include/libspill.hpp $(LIB)
	$(CXX) $(CXXFLAGS) -std=c++20 -o $@ $< $(LIB) $(LDLIBS)

# git add -A has caught a build artefact three times (libspill.a,
# tests/churn_libscratch, libspill.so). Cheaper to check than to remember.
# The Python binding needs the shared object and numpy; skipped without either.
check-python: $(SO)
	@if python3 -c "import numpy" >/dev/null 2>&1; then \
	    python3 python/test_libspill.py; \
	 else \
	    echo "  skipped: numpy not available"; \
	 fi

# Packaging: install through CMake, then build a consumer project against the
# installed package with find_package, in C, C++ and Fortran. Nothing in the
# consumer refers to this source tree, so a broken export or a missing header
# fails here rather than in an adopter's build.
INSTALL_TEST_DIR := $(CURDIR)/.installcheck

check-install:
	@command -v cmake >/dev/null 2>&1 || { echo "  skipped: no cmake"; exit 0; }; \
	 rm -rf $(INSTALL_TEST_DIR); \
	 cmake -S . -B $(INSTALL_TEST_DIR)/build -DLIBSPILL_BUILD_FORTRAN=ON \
	       -DCMAKE_INSTALL_PREFIX=$(INSTALL_TEST_DIR)/prefix >/dev/null && \
	 cmake --build $(INSTALL_TEST_DIR)/build -j4 >/dev/null && \
	 cmake --install $(INSTALL_TEST_DIR)/build >/dev/null && \
	 cmake -S tests/consumer -B $(INSTALL_TEST_DIR)/consumer \
	       -Dlibspill_DIR=$$(dirname $$(find $(INSTALL_TEST_DIR)/prefix -name libspillConfig.cmake)) \
	       >/dev/null && \
	 cmake --build $(INSTALL_TEST_DIR)/consumer -j4 >/dev/null && \
	 LD_LIBRARY_PATH=$$(dirname $$(find $(INSTALL_TEST_DIR)/prefix -name 'libspill.so.0.*')) \
	   sh -c '$(INSTALL_TEST_DIR)/consumer/c_use && \
	          $(INSTALL_TEST_DIR)/consumer/cxx_use && \
	          $(INSTALL_TEST_DIR)/consumer/f_use' && \
	 rm -rf $(INSTALL_TEST_DIR)

check-clean:
	@bad=$$(git ls-files 2>/dev/null | while read f; do \
	          [ -f "$$f" ] && file --mime "$$f" 2>/dev/null | grep -q 'charset=binary' && echo "$$f"; \
	        done); \
	 if [ -n "$$bad" ]; then echo "  tracked binary files:"; echo "$$bad" | sed 's/^/    /'; exit 1; \
	 else echo "  no tracked binaries"; fi
	@if git ls-files 2>/dev/null | xargs grep -l "/hom[e]/" 2>/dev/null | grep -v '^DESIGN.md$$'; then \
	   echo "  ^ absolute local paths in tracked files"; exit 1; \
	 else echo "  no absolute local paths outside DESIGN.md"; fi

check-c: check-clean $(TESTS) $(CXX_TEST)
	@for t in $(TESTS) $(CXX_TEST); do echo "== $$t"; ./$$t || exit 1; done

check: check-c $(FORT_TEST)
	@for t in $(FORT_TEST); do echo "== $$t"; ./$$t || exit 1; done
	@echo "== python binding"; $(MAKE) --no-print-directory check-python
	@echo "== install and consume"; $(MAKE) --no-print-directory check-install

bench: $(BENCH)

clean:
	rm -f $(OBJ) $(DEP) $(LIB) $(SO) $(TESTS) $(BENCH) \
	      $(FORT_OBJ) $(FORT_TEST) fortran/*.mod $(CXX_TEST)

-include $(DEP)

.PHONY: all check check-c check-python check-install check-clean bench clean
