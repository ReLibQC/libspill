! OpenMolcas's DaFile family, reimplemented on libspill.
!
! Unlike the Psi4 port (DESIGN.md §6b), this one cannot treat the address as
! opaque. Psi4 consumers never do arithmetic on psio_address -- `.page` appears
! zero times outside libpsio -- but OpenMolcas callers add byte and word counts
! to iDisk in hundreds of places. The media block length is therefore part of
! the contract, not an implementation detail:
!
!   MBl_wa  = 8    word-addressable units (DaName_wa): iDisk counts 8-byte words
!   MBl_nwa = 512  everything else:        iDisk counts 512-byte blocks
!
! and a transfer that does not fill a whole block rounds the returned cursor UP
! to the next one. Reproducing that exactly is the whole difficulty of this port,
! and the reason it is more than a mechanical substitution.
!
! Two things disappear by construction:
!
!   * Multi_File / MaxFileSize striping (mpdafile.F90 and friends, 328 lines,
!     23 references) exists because a unit could outgrow a file. A libspill
!     store has no such limit, so the split-file machinery has nothing to do --
!     the same bug class §6a deletes from crayio's fixed max_file table.
!   * The shared position array Addr() that §5a identifies as unguarded across
!     some 672 call sites. libspill uses pread/pwrite and carries no file
!     position at all, so the thread-safety problem is not fixed, it is absent.
!
! And one thing becomes real. iOpt 6 and 7 are documented as asynchronous write
! and read; in OpenMolcas they fall through to the identical AixWr/AixRd calls
! as options 1 and 2, so they have never been asynchronous, and no call site in
! the tree uses them. Here they are genuinely asynchronous, with the contract
! that the caller's buffer must stay valid until the next DaFile operation on
! that unit or DaClos, whichever comes first. Adopting overlap is then a
! one-character edit at whichever call sites their authors judge safe.

module daf_ls_state
  use, intrinsic :: iso_c_binding
  use libspill
  implicit none
  public

  integer, parameter :: MxFile = 500
  integer, parameter :: MBl_wa = 8, MBl_nwa = 512

  type :: unit_t
    type(c_ptr) :: store = c_null_ptr
    type(c_ptr) :: req   = c_null_ptr      ! outstanding async request, if any
    integer     :: mbl   = MBl_nwa
    integer(c_int64_t) :: addr = 0         ! cursor in bytes, mirroring Addr()
    logical     :: open  = .false.
    character(len=256) :: name = ' '
  end type unit_t

  type(unit_t), save :: units(MxFile)
  character(kind=c_char), save, target :: dir_buf(512) = c_null_char
  logical, save :: have_dir = .false.

contains

  ! The whole record of a unit lives under one key: OpenMolcas addresses a unit
  ! by offset alone, so there is nothing for a key space to distinguish. The
  ! keyed layer is RunFile, one level up.
  subroutine daf_drain(Lu)
    integer, intent(in) :: Lu
    integer :: rc
    if (c_associated(units(Lu)%req)) then
      rc = ls_wait_f(units(Lu)%req)
      units(Lu)%req = c_null_ptr
      if (rc /= LS_OK) then
        write(6,*) 'DaFile: deferred I/O failed on unit ', Lu, ': ', trim(ls_strerror_f(rc))
        error stop 1
      end if
    end if
  end subroutine daf_drain

end module daf_ls_state

! ---------------------------------------------------------------------------
! The entry points below keep OpenMolcas's signatures exactly, so that call
! sites are untouched.

subroutine daf_ls_set_dir(dir)
  use daf_ls_state
  implicit none
  character(len=*), intent(in) :: dir
  integer :: i, n
  n = min(len_trim(dir), size(dir_buf) - 1)
  do i = 1, n
    dir_buf(i) = dir(i:i)
  end do
  dir_buf(n+1) = c_null_char
  have_dir = (n > 0)
end subroutine daf_ls_set_dir

subroutine DaName_Internal(Lu, Name, wa)
  use daf_ls_state
  implicit none
  integer, intent(in) :: Lu
  character(len=*), intent(in) :: Name
  logical, intent(in) :: wa
  type(ls_opts_t) :: o
  integer :: err

  if ((Lu < 1) .or. (Lu > MxFile)) then
    write(6,*) 'DaName: unit out of range: ', Lu
    error stop 1
  end if
  if (units(Lu)%open) return

  o = ls_defaults()
  ! libspill takes the directory as a C string; the saved target above keeps
  ! the pointer valid for the life of every store.
  if (have_dir) o%dir = c_loc(dir_buf(1))

  units(Lu)%store = ls_open_f(trim(Name), o, err)
  if (err /= LS_OK) then
    write(6,*) 'DaName: cannot open ', trim(Name), ': ', trim(ls_strerror_f(err))
    error stop 1
  end if
  units(Lu)%mbl  = merge(MBl_wa, MBl_nwa, wa)
  units(Lu)%addr = 0
  units(Lu)%open = .true.
  units(Lu)%name = Name
end subroutine DaName_Internal

subroutine DaName(Lu, Name)
  implicit none
  integer, intent(in) :: Lu
  character(len=*), intent(in) :: Name
  call DaName_Internal(Lu, Name, .false.)
end subroutine DaName

subroutine DaName_wa(Lu, Name)
  implicit none
  integer, intent(in) :: Lu
  character(len=*), intent(in) :: Name
  call DaName_Internal(Lu, Name, .true.)
end subroutine DaName_wa

subroutine DaClos(Lu)
  use daf_ls_state
  implicit none
  integer, intent(in) :: Lu
  integer :: rc
  if (.not. units(Lu)%open) return
  call daf_drain(Lu)
  rc = ls_close_f(units(Lu)%store, 0)
  units(Lu)%open = .false.
  units(Lu)%store = c_null_ptr
end subroutine DaClos

subroutine DaEras(Lu)
  implicit none
  integer, intent(in) :: Lu
  call DaClos(Lu)
end subroutine DaEras

! bDaFile: byte-level transfer. iDisk is in BYTES here, as in OpenMolcas.
subroutine bDaFile(Lu, iOpt, Buf, lBuf, iDisk)
  use daf_ls_state
  implicit none
  integer, intent(in) :: Lu, iOpt, lBuf
  character, intent(inout) :: Buf(*)
  integer, intent(inout) :: iDisk
  integer :: rc
  integer(c_int64_t) :: off
  type(c_ptr) :: bp

  if (.not. units(Lu)%open) then
    write(6,*) 'bDaFile: unit not open: ', Lu
    error stop 1
  end if

  select case (iOpt)
  case (5, 10)                      ! rewind
    call daf_drain(Lu)
    units(Lu)%addr = 0
    iDisk = 0
    return
  case (0)                          ! dummy write: advance the cursor only
    units(Lu)%addr = int(iDisk, c_int64_t) + lBuf
    iDisk = int(units(Lu)%addr)
    return
  case (8)                          ! position at end of file
    call daf_drain(Lu)
    block
      integer(c_int64_t) :: n
      rc = ls_size_f(units(Lu)%store, 'd', n)
      if (rc /= LS_OK) n = 0
      iDisk = int(n)
    end block
    return
  end select

  ! Any real transfer orders itself after an outstanding asynchronous one, which
  ! is also what releases the caller's previous buffer.
  call daf_drain(Lu)
  off = int(iDisk, c_int64_t)
  bp = daf_addr(Buf)

  select case (iOpt)
  case (1)
    rc = ls_write_f(units(Lu)%store, 'd', off, lBuf, bp)
  case (2)
    rc = ls_read_f (units(Lu)%store, 'd', off, lBuf, bp)
  case (6)
    rc = ls_awrite_f(units(Lu)%store, 'd', off, lBuf, bp, units(Lu)%req)
  case (7)
    rc = ls_aread_f (units(Lu)%store, 'd', off, lBuf, bp, units(Lu)%req)
  case (99)                          ! dummy read: does the data exist?
    block
      integer(c_int64_t) :: n
      rc = ls_size_f(units(Lu)%store, 'd', n)
      Buf(1) = char(0)
      if ((rc == LS_OK) .and. (n >= off + lBuf)) Buf(1) = char(1)
    end block
    return
  case default
    write(6,*) 'bDaFile: unknown option ', iOpt
    error stop 1
  end select

  if (rc /= LS_OK) then
    write(6,*) 'bDaFile: unit ', Lu, ' option ', iOpt, ': ', trim(ls_strerror_f(rc))
    error stop 1
  end if

  iDisk = iDisk + lBuf
  units(Lu)%addr = int(iDisk, c_int64_t)

contains

  ! A dummy argument must be TARGET before c_loc may be applied to it, and
  ! OpenMolcas's callers do not declare their buffers so. This is the same
  ! internal-subroutine idiom OpenMolcas itself uses in dDaFile_Internal.
  function daf_addr(B) result(p)
    character, target, intent(in) :: B(*)
    type(c_ptr) :: p
    p = c_loc(B(1))
  end function daf_addr

end subroutine bDaFile

subroutine DaFile(Lu, iOpt, Buf, lBuf, iDisk)
  implicit none
  integer, intent(in) :: Lu, iOpt, lBuf
  character, intent(inout) :: Buf(*)
  integer, intent(inout) :: iDisk
  call bDaFile(Lu, iOpt, Buf, lBuf, iDisk)
end subroutine DaFile

! dDaFile: lBuf counts doubles, iDisk counts MBL-sized blocks. The rounding of
! the returned cursor is OpenMolcas's, reproduced exactly -- callers add counts
! to it, so it is observable behaviour rather than bookkeeping.
subroutine dDaFile(Lu, iOpt, Buf, lBuf, iDisk)
  use, intrinsic :: iso_c_binding
  use daf_ls_state
  implicit none
  integer, intent(in) :: Lu, iOpt, lBuf
  real(c_double), target, intent(inout) :: Buf(*)
  integer, intent(inout) :: iDisk
  character, pointer :: cBuf(:)
  integer :: nbytes, bdisk, mbl

  mbl = units(Lu)%mbl
  nbytes = lBuf * 8
  bdisk  = iDisk * mbl
  call c_f_pointer(c_loc(Buf(1)), cBuf, [nbytes])
  call bDaFile(Lu, iOpt, cBuf, nbytes, bdisk)
  nullify(cBuf)
  iDisk = (bdisk + mbl - 1) / mbl
end subroutine dDaFile

subroutine iDaFile(Lu, iOpt, Buf, lBuf, iDisk)
  use, intrinsic :: iso_c_binding
  use daf_ls_state
  implicit none
  integer, intent(in) :: Lu, iOpt, lBuf
  integer(c_int32_t), target, intent(inout) :: Buf(*)
  integer, intent(inout) :: iDisk
  character, pointer :: cBuf(:)
  integer :: nbytes, bdisk, mbl

  mbl = units(Lu)%mbl
  nbytes = lBuf * 4
  bdisk  = iDisk * mbl
  call c_f_pointer(c_loc(Buf(1)), cBuf, [nbytes])
  call bDaFile(Lu, iOpt, cBuf, nbytes, bdisk)
  nullify(cBuf)
  iDisk = (bdisk + mbl - 1) / mbl
end subroutine iDaFile
