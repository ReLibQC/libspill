! libscratch -- Fortran interface.
!
! DESIGN.md §4a: "The C entry points remain public and supported -- Fortran needs
! them and ABI stability is the point -- but no C++ or Python consumer should
! have to touch them." Fortran callers should not have to either, so this module
! wraps the C ABI rather than exposing it: keys are Fortran character strings,
! buffers are passed by reference, and errors come back as an integer that
! ls_strerror_f turns into text.
!
! Everything here is bind(c) against include/libscratch.h. The struct layout of
! ls_opts is mirrored exactly; its `version` field is what lets this module keep
! working when the C struct grows.

module libscratch
  use, intrinsic :: iso_c_binding
  implicit none
  private

  public :: ls_opts_t, ls_defaults, ls_open_f, ls_close_f
  public :: ls_write_f, ls_read_f, ls_reserve_f
  public :: ls_exists_f, ls_size_f, ls_erase_f
  public :: ls_awrite_f, ls_aread_f, ls_wait_f, ls_test_f
  public :: ls_strerror_f
  public :: LS_OK, LS_ERR_NOKEY, LS_ERR_RANGE, LS_ERR_INVAL, LS_ERR_MODE
  public :: LS_ERR_BACKEND, LS_ERR_BUSY, LS_ERR_CORRUPT
  public :: LS_POSIX, LS_HDF5, LS_EXPLICIT, LS_MAPPED, LS_LOCAL, LS_PER_RANK
  public :: LS_KEY_MAX

  integer(c_int), parameter :: LS_OK          =     0
  integer(c_int), parameter :: LS_ERR_NOKEY   = -1000
  integer(c_int), parameter :: LS_ERR_RANGE   = -1001
  integer(c_int), parameter :: LS_ERR_INVAL   = -1002
  integer(c_int), parameter :: LS_ERR_MODE    = -1003
  integer(c_int), parameter :: LS_ERR_BACKEND = -1004
  integer(c_int), parameter :: LS_ERR_BUSY    = -1005
  integer(c_int), parameter :: LS_ERR_CORRUPT = -1006

  integer(c_int), parameter :: LS_POSIX = 0, LS_HDF5 = 1
  integer(c_int), parameter :: LS_EXPLICIT = 0, LS_MAPPED = 1
  integer(c_int), parameter :: LS_LOCAL = 0, LS_PER_RANK = 1
  integer, parameter :: LS_KEY_MAX = 255

  ! Mirrors ls_opts. Obtain one from ls_defaults, never by declaring and filling.
  type, bind(c) :: ls_opts_t
    integer(c_int32_t) :: version
    integer(c_int)     :: backend
    integer(c_int)     :: mode
    integer(c_int)     :: parallel
    integer(c_int)     :: rank
    integer(c_size_t)  :: memory_budget
    type(c_ptr)        :: dir
    integer(c_int)     :: direct_io
    type(c_funptr)     :: log
    type(c_ptr)        :: log_ctx
  end type ls_opts_t

  interface
    subroutine c_opts_default(o) bind(c, name='ls_opts_default')
      import :: ls_opts_t
      type(ls_opts_t), intent(out) :: o
    end subroutine

    function c_open(name, o, err) bind(c, name='ls_open') result(s)
      import :: c_ptr, c_char, c_int, ls_opts_t
      character(kind=c_char), intent(in) :: name(*)
      type(ls_opts_t), intent(in) :: o
      integer(c_int), intent(out) :: err
      type(c_ptr) :: s
    end function

    function c_close(s, keep) bind(c, name='ls_close') result(rc)
      import :: c_ptr, c_int
      type(c_ptr), value :: s
      integer(c_int), value :: keep
      integer(c_int) :: rc
    end function

    function c_write(s, key, off, n, buf) bind(c, name='ls_write') result(rc)
      import :: c_ptr, c_char, c_int, c_int64_t, c_size_t
      type(c_ptr), value :: s
      character(kind=c_char), intent(in) :: key(*)
      integer(c_int64_t), value :: off
      integer(c_size_t), value :: n
      type(c_ptr), value :: buf
      integer(c_int) :: rc
    end function

    function c_read(s, key, off, n, buf) bind(c, name='ls_read') result(rc)
      import :: c_ptr, c_char, c_int, c_int64_t, c_size_t
      type(c_ptr), value :: s
      character(kind=c_char), intent(in) :: key(*)
      integer(c_int64_t), value :: off
      integer(c_size_t), value :: n
      type(c_ptr), value :: buf
      integer(c_int) :: rc
    end function

    function c_reserve(s, key, n) bind(c, name='ls_reserve') result(rc)
      import :: c_ptr, c_char, c_int, c_int64_t
      type(c_ptr), value :: s
      character(kind=c_char), intent(in) :: key(*)
      integer(c_int64_t), value :: n
      integer(c_int) :: rc
    end function

    function c_exists(s, key, found) bind(c, name='ls_exists') result(rc)
      import :: c_ptr, c_char, c_int
      type(c_ptr), value :: s
      character(kind=c_char), intent(in) :: key(*)
      integer(c_int), intent(out) :: found
      integer(c_int) :: rc
    end function

    function c_size(s, key, n) bind(c, name='ls_size') result(rc)
      import :: c_ptr, c_char, c_int, c_int64_t
      type(c_ptr), value :: s
      character(kind=c_char), intent(in) :: key(*)
      integer(c_int64_t), intent(out) :: n
      integer(c_int) :: rc
    end function

    function c_erase(s, key) bind(c, name='ls_erase') result(rc)
      import :: c_ptr, c_char, c_int
      type(c_ptr), value :: s
      character(kind=c_char), intent(in) :: key(*)
      integer(c_int) :: rc
    end function

    function c_awrite(s, key, off, n, buf, req) bind(c, name='ls_awrite') result(rc)
      import :: c_ptr, c_char, c_int, c_int64_t, c_size_t
      type(c_ptr), value :: s
      character(kind=c_char), intent(in) :: key(*)
      integer(c_int64_t), value :: off
      integer(c_size_t), value :: n
      type(c_ptr), value :: buf
      type(c_ptr), intent(out) :: req
      integer(c_int) :: rc
    end function

    function c_aread(s, key, off, n, buf, req) bind(c, name='ls_aread') result(rc)
      import :: c_ptr, c_char, c_int, c_int64_t, c_size_t
      type(c_ptr), value :: s
      character(kind=c_char), intent(in) :: key(*)
      integer(c_int64_t), value :: off
      integer(c_size_t), value :: n
      type(c_ptr), value :: buf
      type(c_ptr), intent(out) :: req
      integer(c_int) :: rc
    end function

    function c_wait(req) bind(c, name='ls_wait') result(rc)
      import :: c_ptr, c_int
      type(c_ptr), value :: req
      integer(c_int) :: rc
    end function

    function c_test(req, done) bind(c, name='ls_test') result(rc)
      import :: c_ptr, c_int
      type(c_ptr), value :: req
      integer(c_int), intent(out) :: done
      integer(c_int) :: rc
    end function

    function c_strerror(err, buf, buflen) bind(c, name='ls_strerror') result(p)
      import :: c_ptr, c_char, c_int, c_size_t
      integer(c_int), value :: err
      character(kind=c_char), intent(inout) :: buf(*)
      integer(c_size_t), value :: buflen
      type(c_ptr) :: p
    end function
  end interface

contains

  ! Fortran strings are not NUL-terminated; every key crosses the boundary
  ! through here, which is the only fiddly part of the binding.
  pure function cstr(s) result(c)
    character(len=*), intent(in) :: s
    character(kind=c_char, len=1), allocatable :: c(:)
    integer :: i, n
    n = len_trim(s)
    allocate(c(n+1))
    do i = 1, n
      c(i) = s(i:i)
    end do
    c(n+1) = c_null_char
  end function cstr

  function ls_defaults() result(o)
    type(ls_opts_t) :: o
    call c_opts_default(o)
  end function ls_defaults

  function ls_open_f(name, o, err) result(s)
    character(len=*), intent(in) :: name
    type(ls_opts_t), intent(in) :: o
    integer, intent(out) :: err
    type(c_ptr) :: s
    integer(c_int) :: e
    s = c_open(cstr(name), o, e)
    err = int(e)
  end function ls_open_f

  function ls_close_f(s, keep) result(rc)
    type(c_ptr), intent(in) :: s
    integer, intent(in) :: keep
    integer :: rc
    rc = int(c_close(s, int(keep, c_int)))
  end function ls_close_f

  function ls_write_f(s, key, off, nbytes, buf) result(rc)
    type(c_ptr), intent(in) :: s
    character(len=*), intent(in) :: key
    integer(c_int64_t), intent(in) :: off
    integer, intent(in) :: nbytes
    type(c_ptr), intent(in) :: buf
    integer :: rc
    rc = int(c_write(s, cstr(key), off, int(nbytes, c_size_t), buf))
  end function ls_write_f

  function ls_read_f(s, key, off, nbytes, buf) result(rc)
    type(c_ptr), intent(in) :: s
    character(len=*), intent(in) :: key
    integer(c_int64_t), intent(in) :: off
    integer, intent(in) :: nbytes
    type(c_ptr), intent(in) :: buf
    integer :: rc
    rc = int(c_read(s, cstr(key), off, int(nbytes, c_size_t), buf))
  end function ls_read_f

  function ls_reserve_f(s, key, nbytes) result(rc)
    type(c_ptr), intent(in) :: s
    character(len=*), intent(in) :: key
    integer(c_int64_t), intent(in) :: nbytes
    integer :: rc
    rc = int(c_reserve(s, cstr(key), nbytes))
  end function ls_reserve_f

  function ls_exists_f(s, key, found) result(rc)
    type(c_ptr), intent(in) :: s
    character(len=*), intent(in) :: key
    logical, intent(out) :: found
    integer :: rc
    integer(c_int) :: f
    rc = int(c_exists(s, cstr(key), f))
    found = (f /= 0)
  end function ls_exists_f

  function ls_size_f(s, key, nbytes) result(rc)
    type(c_ptr), intent(in) :: s
    character(len=*), intent(in) :: key
    integer(c_int64_t), intent(out) :: nbytes
    integer :: rc
    rc = int(c_size(s, cstr(key), nbytes))
  end function ls_size_f

  function ls_erase_f(s, key) result(rc)
    type(c_ptr), intent(in) :: s
    character(len=*), intent(in) :: key
    integer :: rc
    rc = int(c_erase(s, cstr(key)))
  end function ls_erase_f

  function ls_awrite_f(s, key, off, nbytes, buf, req) result(rc)
    type(c_ptr), intent(in) :: s
    character(len=*), intent(in) :: key
    integer(c_int64_t), intent(in) :: off
    integer, intent(in) :: nbytes
    type(c_ptr), intent(in) :: buf
    type(c_ptr), intent(out) :: req
    integer :: rc
    rc = int(c_awrite(s, cstr(key), off, int(nbytes, c_size_t), buf, req))
  end function ls_awrite_f

  function ls_aread_f(s, key, off, nbytes, buf, req) result(rc)
    type(c_ptr), intent(in) :: s
    character(len=*), intent(in) :: key
    integer(c_int64_t), intent(in) :: off
    integer, intent(in) :: nbytes
    type(c_ptr), intent(in) :: buf
    type(c_ptr), intent(out) :: req
    integer :: rc
    rc = int(c_aread(s, cstr(key), off, int(nbytes, c_size_t), buf, req))
  end function ls_aread_f

  function ls_wait_f(req) result(rc)
    type(c_ptr), intent(in) :: req
    integer :: rc
    rc = int(c_wait(req))
  end function ls_wait_f

  function ls_test_f(req, done) result(rc)
    type(c_ptr), intent(in) :: req
    logical, intent(out) :: done
    integer :: rc
    integer(c_int) :: d
    rc = int(c_test(req, d))
    done = (d /= 0)
  end function ls_test_f

  function ls_strerror_f(err) result(text)
    integer, intent(in) :: err
    character(len=128) :: text
    character(kind=c_char, len=1) :: buf(128)
    type(c_ptr) :: p
    integer :: i
    buf = c_null_char
    p = c_strerror(int(err, c_int), buf, int(128, c_size_t))
    text = ' '
    do i = 1, 128
      if (buf(i) == c_null_char) exit
      text(i:i) = buf(i)
    end do
  end function ls_strerror_f

end module libscratch
