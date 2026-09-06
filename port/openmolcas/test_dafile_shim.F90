! SPDX-License-Identifier: BSD-3-Clause
! Exercises the DaFile shim the way OpenMolcas exercises io_util: a threaded
! iDisk cursor, both media block lengths, and the block rounding that callers
! add counts to.
program test_dafile_shim
  use, intrinsic :: iso_c_binding
  use molcas_kinds, only: iwp
  implicit none

  ! Declared with OpenMolcas's own integer kind, because that is what its call
  ! sites pass. A test using the compiler's default integer would not exercise
  ! the mismatch that kind exists to prevent.
  integer(iwp), parameter :: LU_WA = 20, LU_NWA = 21
  integer :: ntest = 0, nfail = 0
  real(c_double), target :: a(4096), b(4096)
  integer(c_int32_t), target :: ia(256), ib(256)
  integer(iwp) :: iDisk, iDisk0
  integer :: i
  character(len=256) :: dir
  logical :: good

  call get_environment_variable('TMPDIR', dir)
  if (len_trim(dir) == 0) dir = '/tmp'
  call daf_ls_set_dir(trim(dir))

  write(6,'(a)') 'OpenMolcas DaFile shim on libspill'

  do i = 1, 4096
    a(i) = 1.0d0 + 0.25d0 * i
  end do

  ! ---- word-addressable unit: MBL = 8, so iDisk counts doubles ----
  call DaName_wa(LU_WA, 'molcas_wa')
  iDisk = 0
  call dDaFile(LU_WA, 1_iwp, a, 4096_iwp, iDisk)
  call check(iDisk == 4096, 'wa: cursor advances one unit per double')

  iDisk = 0
  b = 0.0d0
  call dDaFile(LU_WA, 2_iwp, b, 4096_iwp, iDisk)
  good = .true.
  do i = 1, 4096
    if (b(i) /= a(i)) good = .false.
  end do
  call check(good, 'wa: 4096 doubles round-trip exactly')

  ! a second record appended at the threaded cursor, the dominant idiom
  iDisk0 = iDisk
  call dDaFile(LU_WA, 1_iwp, a, 1024_iwp, iDisk)
  call check(iDisk == iDisk0 + 1024, 'wa: appending threads the cursor on')
  b = 0.0d0
  iDisk = iDisk0
  call dDaFile(LU_WA, 2_iwp, b, 1024_iwp, iDisk)
  good = .true.
  do i = 1, 1024
    if (b(i) /= a(i)) good = .false.
  end do
  call check(good, 'wa: the appended record reads back exactly')

  ! ---- iOpt 0 is a dummy write: cursor only, no I/O ----
  iDisk0 = iDisk
  call dDaFile(LU_WA, 0_iwp, b, 512_iwp, iDisk)
  call check(iDisk == iDisk0 + 512, 'iOpt=0 advances the cursor without writing')

  ! ---- iOpt 5 rewinds ----
  call dDaFile(LU_WA, 5_iwp, b, 0_iwp, iDisk)
  call check(iDisk == 0, 'iOpt=5 rewinds the cursor')

  ! ---- genuinely asynchronous write and read, which OpenMolcas declares
  !      (iOpt 6 and 7) but implements as ordinary synchronous calls ----
  iDisk = 0
  call dDaFile(LU_WA, 6_iwp, a, 4096_iwp, iDisk)
  call check(iDisk == 4096, 'iOpt=6 advances the cursor like a write')
  b = 0.0d0
  iDisk = 0
  call dDaFile(LU_WA, 7_iwp, b, 4096_iwp, iDisk)      ! drains the write first
  iDisk = 0
  call dDaFile(LU_WA, 2_iwp, b, 4096_iwp, iDisk)      ! drains the read, then reads
  good = .true.
  do i = 1, 4096
    if (b(i) /= a(i)) good = .false.
  end do
  call check(good, 'asynchronous 6/7 deliver the same bytes as 1/2')

  ! ---- the integer kind itself ----
  ! DaFile and its family are EXTERNAL subroutines: OpenMolcas gives them no
  ! explicit interface, so a shim declaring its dummies with the wrong integer
  ! kind compiles silently everywhere and is wrong at run time. This value does
  ! not fit in 32 bits, so it truncates if the interface width ever drifts.
  iDisk = 3000000000_iwp
  call dDaFile(LU_WA, 0_iwp, b, 0_iwp, iDisk)
  call check(iDisk == 3000000000_iwp, 'a disk address above 2^31 survives the interface')

  call DaClos(LU_WA)

  ! ---- non-word-addressable unit: MBL = 512, and the rounding it implies ----
  call DaName(LU_NWA, 'molcas_nwa')
  iDisk = 0
  call dDaFile(LU_NWA, 1_iwp, a, 100_iwp, iDisk)
  ! 100 doubles = 800 bytes; (800 + 511)/512 = 2 blocks
  call check(iDisk == 2, 'nwa: a partial block rounds the cursor up to 2')

  call dDaFile(LU_NWA, 1_iwp, a(101), 100_iwp, iDisk)
  call check(iDisk == 4, 'nwa: the next record starts on a block boundary')

  b = 0.0d0
  iDisk = 0
  call dDaFile(LU_NWA, 2_iwp, b, 100_iwp, iDisk)
  good = .true.
  do i = 1, 100
    if (b(i) /= a(i)) good = .false.
  end do
  call check(good, 'nwa: first record reads back exactly')

  b = 0.0d0
  iDisk = 2
  call dDaFile(LU_NWA, 2_iwp, b, 100_iwp, iDisk)
  good = .true.
  do i = 1, 100
    if (b(i) /= a(100+i)) good = .false.
  end do
  call check(good, 'nwa: second record reads back from its own block')

  ! ---- integer transfers ----
  do i = 1, 256
    ia(i) = 7 * i
  end do
  iDisk = 0
  call iDaFile(LU_NWA, 1_iwp, ia, 256_iwp, iDisk)
  ib = 0
  iDisk = 0
  call iDaFile(LU_NWA, 2_iwp, ib, 256_iwp, iDisk)
  good = .true.
  do i = 1, 256
    if (ib(i) /= ia(i)) good = .false.
  end do
  call check(good, 'iDaFile round-trips integers')

  call DaClos(LU_NWA)

  write(6,'(i0,a,i0,a)') ntest, ' checks, ', nfail, ' failed'
  if (nfail /= 0) error stop 1

contains

  subroutine check(cond, what)
    logical, intent(in) :: cond
    character(len=*), intent(in) :: what
    ntest = ntest + 1
    if (cond) then
      write(6,'(a,a)') '  [PASS] ', what
    else
      write(6,'(a,a)') '  [FAIL] ', what
      nfail = nfail + 1
    end if
  end subroutine check

end program test_dafile_shim
