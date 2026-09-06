! SPDX-License-Identifier: BSD-3-Clause
! The integer and real kinds OpenMolcas's interfaces are declared with.
!
! In OpenMolcas this module does not exist: the shims say
! `use Definitions, only: iwp, wp` and get these from src/system_util/
! definitions.F90, which selects iwp = int64 or int32 from the build's
! integer width. It is reproduced here so the shims compile outside the tree,
! and it defaults to int64 because that is OpenMolcas's default build.
!
! This is not a detail. Every interface-visible argument of DaFile, gxWrRun and
! the rest is declared integer(kind=iwp) there. A shim declaring them plain
! `integer` -- int32 under every compiler that matters -- would take int64
! actuals into int32 dummies and be wrong from the first call, silently for
! small values.
module molcas_kinds
  use, intrinsic :: iso_fortran_env, only: int32, int64, real64
  implicit none
  public

#ifdef MOLCAS_I4
  integer, parameter :: iwp = int32
#else
  integer, parameter :: iwp = int64
#endif
  integer, parameter :: wp = real64
end module molcas_kinds
