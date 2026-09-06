# A consumer project, deliberately outside the libspill build.
#
# This is the packaging check: DESIGN.md §1's criterion is deprecation, and a
# maintainer cannot delete their own layer in favour of one they cannot add to
# their build. It uses find_package and the exported targets exactly as Psi4 or
# OpenMolcas would, in all three compiled languages, and nothing here refers to
# the libspill source tree.
cmake_minimum_required(VERSION 3.16)
project(consumer LANGUAGES C CXX Fortran)
find_package(libspill 0.1 REQUIRED)

add_executable(c_use c_use.c)
target_link_libraries(c_use PRIVATE libspill::spill)

add_executable(cxx_use cxx_use.cc)
target_link_libraries(cxx_use PRIVATE libspill::spill)
target_compile_features(cxx_use PRIVATE cxx_std_20)

add_executable(f_use f_use.F90)
target_link_libraries(f_use PRIVATE libspill::spill_f)
