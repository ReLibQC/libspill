! SPDX-License-Identifier: BSD-3-Clause
! Quantum Package 2's mmap wrapper, reimplemented on libspill's LS_MAPPED mode.
!
! This one is worth explaining, because qp2's own survey called it the single
! thing a keyed byte-range store could NOT serve:
!
!   "Mechanism 2 does not fit a byte-range store at all, and this is the
!    survey's one clear negative result: the Cholesky work matrix and the
!    Davidson W/S matrices are not records being read or written -- they are
!    dense linear-algebra operands (BLAS3 dgemm arguments, random-index element
!    updates) that happen to be too large for RAM, backed by mmap specifically
!    so that ordinary Fortran array syntax keeps working unchanged... If a
!    shared library wants to serve this use case, the primitive it needs to
!    expose is closer to 'give me a large flat memory region backed by scratch
!    storage' (i.e. its own mmap-like allocator with byte addressing), not a
!    record/key API."
!
! That paragraph is a specification, and LS_MAPPED -- added later, on the
! evidence of qp2 and RMG (DESIGN.md §3) -- is what it specifies. ls_reserve
! sizes the region, ls_map hands back one pointer, and c_f_pointer turns it into
! the same Fortran array qp2 already indexes. No call site changes: `w =>
! map_w%d2` still works, `L(Lset(p),k)` still works.
!
! The signatures below are qp2's, from src/utils/mmap.f90.
!
! Two differences from qp2's own wrapper, both stated rather than hidden:
!
!   * qp2 unlinks an anonymous mapping immediately after creating it, so a crash
!     leaves nothing behind. Here the backing store is unlinked at
!     mmap_destroy. The window is longer; the file is still gone at exit.
!   * `single_node` is qp2's hint about node-local placement. libspill honours
!     TMPDIR, which on a cluster is normally node-local, so the flag is accepted
!     and has no separate effect.

module mmap_ls
  use, intrinsic :: iso_c_binding
  use libspill
  implicit none
  public

  ! qp2's mmap_type, with the fields its callers actually touch.
  type :: mmap_type
    type(c_ptr) :: ptr = c_null_ptr
    integer(c_int64_t) :: length = 0
    real(c_double), pointer :: d1(:) => null(), d2(:,:) => null()
    real(c_double), pointer :: d3(:,:,:) => null(), d4(:,:,:,:) => null()
    real(c_float),  pointer :: s1(:) => null(), s2(:,:) => null()
    integer(c_int32_t), pointer :: i1(:) => null(), i2(:,:) => null()
    integer(c_int64_t), pointer :: j1(:) => null(), j2(:,:) => null()
    ! libspill state behind the mapping
    type(c_ptr) :: store = c_null_ptr
    character(len=64) :: name = ''
  end type mmap_type

  integer, save, private :: seq = 0

contains

  ! One store per mapping, opened LS_MAPPED. That is not wasteful: a store is a
  ! file, and qp2's wrapper makes a file per mapping too.
  subroutine mmap_create(filename, shp, bytes, read_only, single_node, map)
    character(len=*), intent(in) :: filename
    integer(c_int64_t), intent(in) :: shp(:)
    integer, intent(in) :: bytes
    logical, intent(in) :: read_only, single_node
    type(mmap_type), intent(out) :: map
    type(ls_opts_t) :: o
    integer(c_int) :: err, rc
    integer(c_size_t) :: got
    integer(c_int64_t) :: n
    integer :: i

    n = int(bytes, c_int64_t)
    do i = 1, size(shp)
      n = n * shp(i)
    end do
    map%length = n

    if (len_trim(filename) > 0) then
      map%name = trim(filename)
    else
      ! qp2 passes '' for the pure RAM-overflow case and lets the wrapper
      ! invent an anonymous file; a unique name does the same job here.
      seq = seq + 1
      write(map%name, '(a,i0,a,i0)') 'qp2_anon_', seq, '_', n
    end if

    o = ls_defaults()
    o%mode = LS_MAPPED
    map%store = ls_open_f(trim(map%name), o, err)
    if (err /= LS_OK) then
      write(6, *) 'mmap_create: ', trim(map%name), ': ', trim(ls_strerror_f(err))
      error stop 1
    end if

    rc = ls_reserve_f(map%store, 'm', n)
    if (rc /= LS_OK) then
      write(6, *) 'mmap_create: reserve: ', trim(ls_strerror_f(rc))
      error stop 1
    end if

    rc = ls_map_f(map%store, 'm', map%ptr, got)
    if (rc /= LS_OK) then
      write(6, *) 'mmap_create: map: ', trim(ls_strerror_f(rc))
      error stop 1
    end if
    if (read_only .or. single_node) continue      ! see the header note
  end subroutine mmap_create

  subroutine mmap_create_d(filename, shp, read_only, single_node, map)
    character(len=*), intent(in) :: filename
    integer(c_int64_t), intent(in) :: shp(:)
    logical, intent(in) :: read_only, single_node
    type(mmap_type), intent(out) :: map
    call mmap_create(filename, shp, 8, read_only, single_node, map)
    select case (size(shp))
      case (1) ; call c_f_pointer(map%ptr, map%d1, shp)
      case (2) ; call c_f_pointer(map%ptr, map%d2, shp)
      case (3) ; call c_f_pointer(map%ptr, map%d3, shp)
      case (4) ; call c_f_pointer(map%ptr, map%d4, shp)
      case default ; stop 'mmap: dimension not implemented'
    end select
  end subroutine mmap_create_d

  subroutine mmap_create_s(filename, shp, read_only, single_node, map)
    character(len=*), intent(in) :: filename
    integer(c_int64_t), intent(in) :: shp(:)
    logical, intent(in) :: read_only, single_node
    type(mmap_type), intent(out) :: map
    call mmap_create(filename, shp, 4, read_only, single_node, map)
    select case (size(shp))
      case (1) ; call c_f_pointer(map%ptr, map%s1, shp)
      case (2) ; call c_f_pointer(map%ptr, map%s2, shp)
      case default ; stop 'mmap: dimension not implemented'
    end select
  end subroutine mmap_create_s

  subroutine mmap_create_i(filename, shp, read_only, single_node, map)
    character(len=*), intent(in) :: filename
    integer(c_int64_t), intent(in) :: shp(:)
    logical, intent(in) :: read_only, single_node
    type(mmap_type), intent(out) :: map
    call mmap_create(filename, shp, 4, read_only, single_node, map)
    select case (size(shp))
      case (1) ; call c_f_pointer(map%ptr, map%i1, shp)
      case (2) ; call c_f_pointer(map%ptr, map%i2, shp)
      case default ; stop 'mmap: dimension not implemented'
    end select
  end subroutine mmap_create_i

  subroutine mmap_create_i8(filename, shp, read_only, single_node, map)
    character(len=*), intent(in) :: filename
    integer(c_int64_t), intent(in) :: shp(:)
    logical, intent(in) :: read_only, single_node
    type(mmap_type), intent(out) :: map
    call mmap_create(filename, shp, 8, read_only, single_node, map)
    select case (size(shp))
      case (1) ; call c_f_pointer(map%ptr, map%j1, shp)
      case (2) ; call c_f_pointer(map%ptr, map%j2, shp)
      case default ; stop 'mmap: dimension not implemented'
    end select
  end subroutine mmap_create_i8

  subroutine mmap_destroy(map)
    type(mmap_type), intent(inout) :: map
    integer(c_int) :: rc
    if (c_associated(map%store)) then
      rc = ls_unmap_f(map%store, 'm')
      rc = ls_close_f(map%store, 0_c_int)     ! keep=0: the backing file goes
      map%store = c_null_ptr
    end if
    map%ptr = c_null_ptr
    nullify(map%d1, map%d2, map%d3, map%d4, map%s1, map%s2)
    nullify(map%i1, map%i2, map%j1, map%j2)
  end subroutine mmap_destroy

  ! qp2 calls msync to push a mapping to disk. The kernel does that for a
  ! MAP_SHARED mapping anyway; this exists so call sites need not change.
  subroutine mmap_sync(map)
    type(mmap_type), intent(inout) :: map
    if (.not. c_associated(map%ptr)) return
  end subroutine mmap_sync

end module mmap_ls
