! SPDX-License-Identifier: BSD-3-Clause
! Does LS_MAPPED actually serve qp2's mechanism 2 -- the case its own survey
! called "the survey's one clear negative result"?
!
! The test is written the way qp2 uses the wrapper: create, take a Fortran
! pointer, then index it with ordinary array syntax, including the two patterns
! the survey names -- BLAS3 operands and random-index element updates.
program test_mmap_shim
  use, intrinsic :: iso_c_binding
  use mmap_ls
  implicit none

  integer :: ntest = 0, nfail = 0
  type(mmap_type) :: map_w, map_s, map_l, map_c
  real(c_double), pointer :: w(:,:), L(:,:), c(:,:)
  real(c_float), pointer :: s(:,:)
  integer(c_int64_t), parameter :: sze = 512, nst = 8
  integer :: i, j
  logical :: good

  write(6,'(a)') 'qp2 mmap wrapper on libspill LS_MAPPED'

  ! Davidson: diagonalization_hs2_dressed.irp.f:280-281, verbatim in shape.
  call mmap_create_d('', (/ 1_8*sze, 1_8*nst /), .False., .True., map_w)
  call mmap_create_s('', (/ 1_8*sze, 1_8*nst /), .False., .True., map_s)
  w => map_w%d2
  s => map_s%s2
  call check(associated(w) .and. associated(s), 'anonymous mappings give Fortran pointers')
  call check(size(w,1) == sze .and. size(w,2) == nst, '  ... of the requested shape')

  ! A freshly reserved region reads as zero (DESIGN.md §4b).
  good = .true.
  do j = 1, int(nst)
    do i = 1, int(sze)
      if (w(i,j) /= 0.0d0) good = .false.
    end do
  end do
  call check(good, 'a new mapping reads as zero')

  ! Random-index element updates -- the pattern the survey says a record API
  ! cannot express.
  do j = 1, int(nst)
    do i = 1, int(sze)
      w(i,j) = 1000.0d0*j + i
      s(i,j) = real(j, c_float)
    end do
  end do
  good = .true.
  do j = 1, int(nst)
    if (w(1,j) /= 1000.0d0*j + 1) good = .false.
    if (w(int(sze),j) /= 1000.0d0*j + sze) good = .false.
    if (s(int(sze),j) /= real(j, c_float)) good = .false.
  end do
  call check(good, 'element-wise writes through the mapping stick')

  ! Cholesky: a 2-D operand indexed through a pivot list, cholesky.irp.f:185.
  call mmap_create_d('', (/ 1_8*sze, 1_8*sze /), .False., .True., map_l)
  L => map_l%d2
  do i = 1, int(sze)
    L(i,i) = 2.0d0
  end do
  ! and used as a BLAS3 operand, which is the point of having a real array.
  ! C must not alias B, so the result goes to a third mapping -- three mapped
  ! regions in one dgemm is exactly the Cholesky/Davidson shape.
  call mmap_create_d('', (/ 1_8*sze, 1_8*nst /), .False., .True., map_c)
  c => map_c%d2
  call dgemm('N','N', int(sze), int(nst), int(sze), 1.0d0, L, int(sze), &
             w, int(sze), 0.0d0, c, int(sze))
  good = .true.
  do j = 1, int(nst)
    if (abs(c(1,j) - 2.0d0*(1000.0d0*j + 1)) > 1.0d-9) good = .false.
    if (abs(c(int(sze),j) - 2.0d0*(1000.0d0*j + sze)) > 1.0d-9) good = .false.
  end do
  call check(good, 'three mapped regions work as dgemm operands')

  call mmap_destroy(map_c)
  call mmap_destroy(map_l)
  call mmap_destroy(map_s)
  call mmap_destroy(map_w)
  call check(.not. associated(map_w%d2), 'mmap_destroy releases the pointers')

  ! A named mapping, the persistent case (the integral cache).
  call mmap_create_d('qp2_cache', (/ 1_8*64 /), .False., .False., map_w)
  map_w%d1(1) = 42.0d0
  map_w%d1(64) = 43.0d0
  call mmap_sync(map_w)
  call check(map_w%d1(1) == 42.0d0 .and. map_w%d1(64) == 43.0d0, &
             'a named mapping round-trips')
  call mmap_destroy(map_w)

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

end program test_mmap_shim
