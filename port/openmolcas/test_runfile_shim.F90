! SPDX-License-Identifier: BSD-3-Clause
! Exercises the RunFile shim the way OpenMolcas's typed wrappers exercise the
! generic core: gxWrRun / gxRdRun / ffxRun with a 16-character Label and a
! record type.
program test_runfile_shim
  use, intrinsic :: iso_c_binding
  use molcas_kinds, only: iwp
  use runfile_ls, only: TypDbl, TypInt, TypStr, TypUnk, rcOK, rcNotFound
  implicit none

  integer :: ntest = 0, nfail = 0
  ! OpenMolcas's kind, for the same reason as in the DaFile test.
  integer(iwp) :: iRc, n, typ
  integer :: i
  real(c_double), target :: d(1024), dback(1024)
  integer(c_int64_t), target :: iv(256), iback(256)
  character(len=32) :: cs, csback
  character(len=256) :: dir
  logical :: good

  call get_environment_variable('TMPDIR', dir)
  if (len_trim(dir) == 0) dir = '/tmp'
  call runfile_ls_set_dir(trim(dir))
  call NameRun('TESTRUN')

  write(6,'(a)') 'OpenMolcas RunFile generic core on libspill'

  do i = 1, 1024
    d(i) = 1.0d0 + 0.125d0 * i
  end do

  ! ---- the dominant idiom: a labelled double array ----
  call gxWrRun(iRc, 'Last orbitals   ', transfer(d, ' ', 8*1024), 1024_iwp, 0_iwp, TypDbl)
  call check(iRc == rcOK, 'gxWrRun a double array')

  dback = 0.0d0
  call rd_dbl('Last orbitals   ', dback, 1024_iwp, iRc)
  call check(iRc == rcOK, 'gxRdRun it back')
  good = .true.
  do i = 1, 1024
    if (dback(i) /= d(i)) good = .false.
  end do
  call check(good, '  ... byte-exact')

  ! ---- the query RunFile scans 1024 entries to answer ----
  call ffxRun(iRc, 'Last orbitals   ', n, typ, 0_iwp)
  call check(iRc == rcOK .and. n == 1024 .and. typ == TypDbl, &
             'ffxRun reports the item count and the type')

  call ffxRun(iRc, 'Never written   ', n, typ, 0_iwp)
  call check(iRc == rcNotFound .and. n == 0 .and. typ == TypUnk, &
             'ffxRun on an absent label is rcNotFound')

  ! ---- integers and characters go through the same core ----
  do i = 1, 256
    iv(i) = int(7 * i, c_int64_t)
  end do
  call gxWrRun(iRc, 'nSym            ', transfer(iv, ' ', 8*256), 256_iwp, 0_iwp, TypInt)
  call check(iRc == rcOK, 'gxWrRun an integer array')
  iback = 0
  call rd_int('nSym            ', iback, 256_iwp, iRc)
  good = .true.
  do i = 1, 256
    if (iback(i) /= iv(i)) good = .false.
  end do
  call check(iRc == rcOK .and. good, 'gxRdRun the integers back')

  cs = 'SEWARD                          '
  call gxWrRun(iRc, 'Seward Title    ', transfer(cs, ' ', 32), 32_iwp, 0_iwp, TypStr)
  csback = ' '
  call rd_str('Seward Title    ', csback, 32_iwp, iRc)
  call check(iRc == rcOK .and. csback == cs, 'a character record round-trips')

  ! ---- trailing blanks: RunFile compares the full 16-character field, so
  !      these must be the same record here too ----
  call ffxRun(iRc, 'nSym', n, typ, 0_iwp)
  call check(iRc == rcOK .and. n == 256, 'a label differing only in trailing blanks is the same record')

  ! ---- growth past the previous size: the case that abandons space in
  !      RunFile, because Next only ever advances ----
  call gxWrRun(iRc, 'Last orbitals   ', transfer(d, ' ', 8*1024), 1024_iwp, 0_iwp, TypDbl)
  call check(iRc == rcOK, 'rewrite at the same size')
  call gxWrRun(iRc, 'Last orbitals   ', transfer(d, ' ', 8*64), 64_iwp, 0_iwp, TypDbl)
  call ffxRun(iRc, 'Last orbitals   ', n, typ, 0_iwp)
  call check(n == 64, 'shrinking a record updates its item count')
  dback = 0.0d0
  call rd_dbl('Last orbitals   ', dback, 64_iwp, iRc)
  call check(iRc == rcOK .and. dback(1) == d(1) .and. dback(64) == d(64), &
             '  ... and it reads back at the new length')

  ! ---- a type change: RunFile drops the slot and takes fresh space ----
  call gxWrRun(iRc, 'Last orbitals   ', transfer(iv, ' ', 8*64), 64_iwp, 0_iwp, TypInt)
  call ffxRun(iRc, 'Last orbitals   ', n, typ, 0_iwp)
  call check(iRc == rcOK .and. typ == TypInt .and. n == 64, &
             'rewriting a label with a different type changes its type')

  ! ---- persistence across the close that OpenMolcas does per call ----
  call Fin_Run_Use()
  call ffxRun(iRc, 'nSym            ', n, typ, 0_iwp)
  call check(iRc == rcOK .and. n == 256 .and. typ == TypInt, &
             'the table of contents survives close and reopen')

  call space_test()

  call Fin_Run_Use()
  write(6,'(i0,a,i0,a)') ntest, ' checks, ', nfail, ' failed'
  if (nfail /= 0) error stop 1

contains

  subroutine rd_dbl(lab, buf, nD, rc)
    character(len=*), intent(in) :: lab
    real(c_double), intent(out) :: buf(*)
    integer(iwp), intent(in) :: nD
    integer(iwp), intent(out) :: rc
    character :: tmp(int(8*nD))
    call gxRdRun(rc, lab, tmp, nD, 0_iwp, TypDbl)
    buf(1:nD) = transfer(tmp, buf(1:nD))
  end subroutine rd_dbl

  subroutine rd_int(lab, buf, nD, rc)
    character(len=*), intent(in) :: lab
    integer(c_int64_t), intent(out) :: buf(*)
    integer(iwp), intent(in) :: nD
    integer(iwp), intent(out) :: rc
    character :: tmp(int(8*nD))
    call gxRdRun(rc, lab, tmp, nD, 0_iwp, TypInt)
    buf(1:nD) = transfer(tmp, buf(1:nD))
  end subroutine rd_int

  subroutine rd_str(lab, buf, nD, rc)
    character(len=*), intent(in) :: lab
    character(len=*), intent(out) :: buf
    integer(iwp), intent(in) :: nD
    integer(iwp), intent(out) :: rc
    character :: tmp(int(nD))
    integer :: j
    call gxRdRun(rc, lab, tmp, nD, 0_iwp, TypStr)
    buf = ' '
    do j = 1, nD
      buf(j:j) = tmp(j)
    end do
  end subroutine rd_str

  ! What the free list is worth on RunFile's own access pattern.
  !
  ! gxWrRun takes fresh space at RunHdr%Next whenever a record outgrows its
  ! MaxLen, marks the old slot Empty, and never reclaims it -- Next only ever
  ! advances. The RunFile figure below is that algorithm, computed from the
  ! source rather than measured, since running OpenMolcas is not in scope here.
  ! The libspill figure is the file on disk.
  !
  ! The model here assumes MaxLen never decreases, which is OPTIMISTIC: the real
  ! code sets MaxLen = max(NewLen, nData) with NewLen taken from Toc(item)%Len,
  ! the previous *length*, not the previous MaxLen (gxwrrun.F90:100 and :125).
  ! Two consecutive writes at a smaller size therefore make the slot forget
  ! capacity it still holds, and the next modest growth abandons all of it. The
  ! figure below is a lower bound on what RunFile actually wastes.
  subroutine space_test()
    integer, parameter :: NLAB = 8, NCYC = 24
    integer :: c, k
    integer(iwp) :: sizes(NLAB), maxlen(NLAB), nsz
    integer(c_int64_t) :: runfile_next, fsz, live
    character(len=16) :: lab
    real(c_double), target :: buf(4096)

    do k = 1, 4096
      buf(k) = real(k, c_double)
    end do
    maxlen = 0
    sizes = 0
    runfile_next = 0

    do c = 1, NCYC
      do k = 1, NLAB
        nsz = 256 + mod(c * 37 + k * 101, 3500)
        write(lab, '(a,i2.2)') 'GrowArray     ', k
        call gxWrRun(iRc, lab, transfer(buf, ' ', int(8*nsz)), nsz, 0_iwp, TypDbl)
        if (iRc /= rcOK) then
          call check(.false., 'space test write')
          return
        end if
        sizes(k) = nsz
        ! RunFile's own rule, from gxwrrun.F90
        if (nsz > maxlen(k)) then
          runfile_next = runfile_next + int(nsz, c_int64_t) * 8
          maxlen(k) = nsz
        end if
      end do
    end do

    live = 0
    do k = 1, NLAB
      live = live + int(sizes(k), c_int64_t) * 8
    end do

    call Fin_Run_Use()
    inquire(file=trim(dir)//'/TESTRUN.libspill', size=fsz)

    write(6,'(a)') '  space, 8 records x 24 cycles at varying sizes:'
    write(6,'(a,f8.2,a)') '    live data                          ', real(live)/1048576.0, ' MiB'
    write(6,'(a,f8.2,a,f5.2,a)') '    RunFile, lower bound from source ', &
      real(runfile_next)/1048576.0, ' MiB  x', real(runfile_next)/real(live), ' '
    write(6,'(a,f8.2,a,f5.2,a)') '    libspill file (measured)         ', &
      real(fsz)/1048576.0, ' MiB  x', real(fsz)/real(live), ' '
    call check(fsz < runfile_next, 'the free list beats RunFile''s monotonic high-water mark')
  end subroutine space_test

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

end program test_runfile_shim
