# SPDX-License-Identifier: BSD-3-Clause
# libspill -- scratch and out-of-core I/O for electronic-structure codes.
#
# The success criterion in DESIGN.md §1 is deprecation: an existing layer
# deleted from a real code, with its maintainer's agreement. Both lead adopters
# (Psi4, OpenMolcas) build with CMake, so being findable by CMake is not
# packaging polish -- a library nobody can add to their build cannot be adopted
# however well its API fits.
#
#   find_package(libspill 0.1 REQUIRED)
#   target_link_libraries(mycode PRIVATE libspill::spill)
#
# The Fortran binding is built only on request, so that a C or C++ consumer does
# not need a Fortran compiler to use the library.

cmake_minimum_required(VERSION 3.16)
project(libspill
        VERSION 0.1.0
        DESCRIPTION "Scratch and out-of-core I/O for electronic-structure codes"
        LANGUAGES C)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(LIBSPILL_BUILD_FORTRAN "Build the ISO_C_BINDING Fortran module" OFF)
option(LIBSPILL_WITH_HDF5     "Build the optional HDF5 backend (§7b)"    OFF)
option(LIBSPILL_WITH_CRAYIO   "Build the crayio compatibility layer"     OFF)
option(LIBSPILL_CRAYIO_I8     "crayio layer uses 64-bit Fortran INTEGER" OFF)
option(LIBSPILL_BUILD_TESTS   "Build the test programs"                OFF)
option(BUILD_SHARED_LIBS      "Build a shared library"                 ON)

# ---------------------------------------------------------------- the library

add_library(spill
  src/error.c
  src/toc.c
  src/alloc.c
  src/store.c
  src/open.c
  src/async.c)
add_library(libspill::spill ALIAS spill)

target_include_directories(spill
  PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
          $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(spill PRIVATE c_std_99)

# The optional HDF5 backend. Off by default: §7b keeps POSIX the default and the
# only backend that can deliver asynchrony, and HDF5 is here for codes that
# already link it and want scratch files h5ls can read. A library built without
# it still accepts LS_HDF5 at compile time and refuses it at ls_open with
# LS_ERR_BACKEND, so callers need no conditional compilation.
if(LIBSPILL_WITH_HDF5)
  find_package(HDF5 REQUIRED COMPONENTS C)
  target_sources(spill PRIVATE src/hdf5.c)
  target_compile_definitions(spill PRIVATE LIBSPILL_HAVE_HDF5)
  target_link_libraries(spill PRIVATE ${HDF5_C_LIBRARIES})
  target_include_directories(spill PRIVATE ${HDF5_C_INCLUDE_DIRS})
endif()

# _GNU_SOURCE is needed for O_DIRECT, fallocate and MAP_FIXED; it is PRIVATE
# because a consumer of the public header needs none of them.
target_compile_definitions(spill PRIVATE _GNU_SOURCE)

find_package(Threads REQUIRED)
target_link_libraries(spill PUBLIC Threads::Threads)

set_target_properties(spill PROPERTIES
  VERSION   ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  C_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  POSITION_INDEPENDENT_CODE ON
  PUBLIC_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/libspill.h;${CMAKE_CURRENT_SOURCE_DIR}/include/libspill.hpp")

# ------------------------------------------------- crayio compatibility layer
#
# A SEPARATE library, never part of libspill itself, because wopen_, wclose_,
# getwa_ and putwa_ are global Fortran symbols: a code that still has its own
# crayio.c would get a duplicate definition, or worse, silently interpose one on
# the other. Linking this library is how a code opts in, in place of its own
# crayio.o.
#
# LIBSPILL_CRAYIO_I8 must match the Fortran integer width of the code it joins.
# Every argument arrives by pointer, so a mismatch is not a widening: it reads
# bytes the caller never wrote, and being a Fortran external, nothing in the
# toolchain will object. See DESIGN.md §6e and §6g.
if(LIBSPILL_WITH_CRAYIO)
  add_library(spill_crayio port/crayio/crayio_libspill.c)
  add_library(libspill::spill_crayio ALIAS spill_crayio)
  target_link_libraries(spill_crayio PUBLIC spill)
  target_include_directories(spill_crayio
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/port/crayio>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  if(LIBSPILL_CRAYIO_I8)
    target_compile_definitions(spill_crayio PUBLIC VAR_INT64)
  endif()
  set_target_properties(spill_crayio PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    POSITION_INDEPENDENT_CODE ON
    PUBLIC_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/port/crayio/crayio_libspill.h")
endif()

# ---------------------------------------------------------- Fortran binding
#
# Optional: a Fortran consumer needs the .mod file, which is compiler- and
# version-specific, so it is installed under a compiler-tagged directory rather
# than into the shared include path.
if(LIBSPILL_BUILD_FORTRAN)
  enable_language(Fortran)
  add_library(spill_f fortran/libspill.F90)
  add_library(libspill::spill_f ALIAS spill_f)
  target_link_libraries(spill_f PUBLIC spill)
  set(LIBSPILL_MODDIR
      ${CMAKE_INSTALL_INCLUDEDIR}/libspill/${CMAKE_Fortran_COMPILER_ID}-${CMAKE_Fortran_COMPILER_VERSION})
  set_target_properties(spill_f PROPERTIES
    Fortran_MODULE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/mod
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})
  target_include_directories(spill_f
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/mod>
           $<INSTALL_INTERFACE:${LIBSPILL_MODDIR}>)
  install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/mod/ DESTINATION ${LIBSPILL_MODDIR})
endif()

# ------------------------------------------------------------------- install

set(LIBSPILL_TARGETS spill)
if(LIBSPILL_BUILD_FORTRAN)
  list(APPEND LIBSPILL_TARGETS spill_f)
endif()
if(LIBSPILL_WITH_CRAYIO)
  list(APPEND LIBSPILL_TARGETS spill_crayio)
endif()

install(TARGETS ${LIBSPILL_TARGETS}
        EXPORT libspillTargets
        LIBRARY       DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE       DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME       DESTINATION ${CMAKE_INSTALL_BINDIR}
        PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT libspillTargets
        FILE libspillTargets.cmake
        NAMESPACE libspill::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libspill)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/libspillConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/libspillConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libspill)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/libspillConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/libspillConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/libspillConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libspill)

# pkg-config, for the Fortran and autotools consumers that do not use CMake.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/libspill.pc.in
               ${CMAKE_CURRENT_BINARY_DIR}/libspill.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/libspill.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# --------------------------------------------------------------------- tests
#
# The Makefile remains the development driver -- it builds the ports and the
# benchmark, which are not installable artefacts. This covers the library's own
# suite so that a packager can run `ctest`.
if(LIBSPILL_BUILD_TESTS)
  enable_testing()
  foreach(t abi_header_test test_posix churn_libspill test_surveyed test_mapped test_hdf5)
    add_executable(${t} tests/${t}.c)
    target_link_libraries(${t} PRIVATE spill)
    target_compile_definitions(${t} PRIVATE _GNU_SOURCE)
    add_test(NAME ${t} COMMAND ${t})
  endforeach()

  # Built at both Fortran integer widths; a mismatch is invisible to the
  # toolchain, so it has to be caught here.
  foreach(w 0 1)
    if(w EQUAL 0)
      set(tn test_crayio_i4)
    else()
      set(tn test_crayio_i8)
    endif()
    add_executable(${tn} port/crayio/test_crayio.c port/crayio/crayio_libspill.c)
    target_link_libraries(${tn} PRIVATE spill)
    target_include_directories(${tn} PRIVATE port/crayio)
    target_compile_definitions(${tn} PRIVATE _GNU_SOURCE)
    if(w EQUAL 1)
      target_compile_definitions(${tn} PRIVATE VAR_INT64)
    endif()
    add_test(NAME ${tn} COMMAND ${tn})
  endforeach()

  enable_language(CXX)
  add_executable(test_cxx tests/test_cxx.cc)
  target_compile_features(test_cxx PRIVATE cxx_std_20)
  target_link_libraries(test_cxx PRIVATE spill)
  add_test(NAME test_cxx COMMAND test_cxx)
endif()
