! SPDX-License-Identifier: BSD-3-Clause
! The Fortran binding of DESIGN.md §4a, tested directly.
!
! Until the port shims moved out to their own codes, every Fortran check in this
! repository came from one of them -- so the binding itself was only ever tested
! through a consumer. This exercises it on its own terms.
program test_fortran
  use, intrinsic :: iso_c_binding
  use libspill
  implicit none

  integer :: ntest = 0, nfail = 0
  type(ls_opts_t) :: o
  type(c_ptr) :: s, req, addr
  integer(c_int) :: err, rc
  integer(c_int64_t) :: n64, off
  integer(c_size_t) :: an
  real(c_double), target :: a(4096), b(4096)
  integer(c_int32_t), target :: meta(2)
  logical :: found, done
  integer :: i
  character(len=256) :: dir

  call get_environment_variable('TMPDIR', dir)
  if (len_trim(dir) == 0) dir = '/tmp'

  write(6,'(a)') 'libspill Fortran binding'

  do i = 1, 4096
    a(i) = 1.0d0 + 0.25d0 * i
  end do

  ! ---- open, write, read ----
  o = ls_defaults()
  s = ls_open_f('fbind', o, err)
  call check(err == LS_OK .and. c_associated(s), 'ls_open_f')

  rc = ls_write_f(s, 'amps', 0_c_int64_t, int(8*4096, c_size_t), c_loc(a))
  call check(rc == LS_OK, 'ls_write_f')

  b = 0.0d0
  rc = ls_read_f(s, 'amps', 0_c_int64_t, int(8*4096, c_size_t), c_loc(b))
  call check(rc == LS_OK .and. all(b == a), 'ls_read_f round-trips exactly')

  ! ---- table of contents ----
  rc = ls_exists_f(s, 'amps', found)
  call check(rc == LS_OK .and. found, 'ls_exists_f')
  rc = ls_size_f(s, 'amps', n64)
  call check(rc == LS_OK .and. n64 == 8*4096, 'ls_size_f reports bytes')
  rc = ls_reserve_f(s, 'resv', 8192_c_int64_t)
  call check(rc == LS_OK, 'ls_reserve_f')
  b = 1.0d0
  rc = ls_read_f(s, 'resv', 0_c_int64_t, int(8192, c_size_t), c_loc(b))
  call check(rc == LS_OK .and. all(b(1:1024) == 0.0d0), '  ... reserved space reads as zero')
  rc = ls_erase_f(s, 'resv')
  call check(rc == LS_OK, 'ls_erase_f')
  rc = ls_exists_f(s, 'resv', found)
  call check(.not. found, '  ... and it is gone')

  ! ---- append ----
  rc = ls_append_f(s, 'stream', int(8*1024, c_size_t), c_loc(a), off)
  call check(rc == LS_OK .and. off == 0, 'ls_append_f to a new key is offset 0')
  rc = ls_append_f(s, 'stream', int(8*1024, c_size_t), c_loc(a), off)
  call check(rc == LS_OK .and. off == 8*1024, '  ... then the previous end')

  ! ---- attributes ----
  meta(1) = 2
  meta(2) = 4096
  rc = ls_set_attr_f(s, 'amps', c_loc(meta), 8_c_size_t)
  call check(rc == LS_OK, 'ls_set_attr_f')
  meta = 0
  an = 8
  rc = ls_get_attr_f(s, 'amps', c_loc(meta), an)
  call check(rc == LS_OK .and. an == 8 .and. meta(1) == 2 .and. meta(2) == 4096, &
             'ls_get_attr_f returns the blob unchanged')

  ! ---- asynchronous ----
  b = 0.0d0
  rc = ls_aread_f(s, 'amps', 0_c_int64_t, int(8*4096, c_size_t), c_loc(b), req)
  call check(rc == LS_OK, 'ls_aread_f accepted')
  rc = ls_test_f(req, done)
  call check(rc == LS_OK, 'ls_test_f')
  rc = ls_wait_f(req)
  call check(rc == LS_OK .and. all(b == a), 'ls_wait_f, and the data arrived')

  ! ---- errors carry a message ----
  rc = ls_read_f(s, 'absent', 0_c_int64_t, 8_c_size_t, c_loc(b))
  call check(rc == LS_ERR_NOKEY, 'a missing key gives LS_ERR_NOKEY')
  call check(len_trim(ls_strerror_f(rc)) > 0, '  ... and ls_strerror_f describes it')

  rc = ls_close_f(s, 1_c_int)
  call check(rc == LS_OK, 'ls_close_f with keep=1')

  ! ---- LS_MAPPED, reachable from Fortran only through ls_map_f ----
  o = ls_defaults()
  o%mode = LS_MAPPED
  s = ls_open_f('fbind', o, err)
  call check(err == LS_OK, 'reopen as LS_MAPPED')
  if (err == LS_OK) then
    block
      real(c_double), pointer :: p(:)
      rc = ls_map_f(s, 'amps', addr, an)
      call check(rc == LS_OK .and. an == 8*4096, 'ls_map_f')
      call c_f_pointer(addr, p, [4096])
      call check(all(p == a), '  ... the mapping shows the stored values')
      p(1) = -1.0d0
      rc = ls_unmap_f(s, 'amps')
      call check(rc == LS_OK, 'ls_unmap_f')
      rc = ls_close_f(s, 1_c_int)
    end block
  end if

  o = ls_defaults()
  s = ls_open_f('fbind', o, err)
  rc = ls_read_f(s, 'amps', 0_c_int64_t, 8_c_size_t, c_loc(b))
  call check(b(1) == -1.0d0, 'a write through the mapping reached the store')
  rc = ls_close_f(s, 0_c_int)

  ! exact_name, which also checks that ls_opts_t still mirrors the C struct:
  ! the field is last, so a layout drift shows up here and nowhere else.
  o = ls_defaults()
  o%exact_name = 1_c_int
  rc = ls_store_exists_f('RUNFILE_F', o, found)
  call check(rc == LS_OK .and. .not. found, 'ls_store_exists_f on an absent store')
  s = ls_open_f('RUNFILE_F', o, err)
  call check(err == LS_OK, 'an exact-named store opens')
  rc = ls_write_f(s, 'k', 0_c_int64_t, int(8*16, c_size_t), c_loc(a))
  rc = ls_close_f(s, 1_c_int)
  rc = ls_store_exists_f('RUNFILE_F', o, found)
  call check(rc == LS_OK .and. found, '  ... and ls_store_exists_f then finds it')
  s = ls_open_f('RUNFILE_F', o, err)
  b = 0.0d0
  rc = ls_read_f(s, 'k', 0_c_int64_t, int(8*16, c_size_t), c_loc(b))
  call check(rc == LS_OK .and. all(b(1:16) == a(1:16)), '  ... and it round-trips')
  rc = ls_close_f(s, 0_c_int)

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

end program test_fortran
