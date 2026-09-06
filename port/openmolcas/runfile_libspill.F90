! OpenMolcas's RunFile generic core, reimplemented on libspill.
!
! This is the port §6c said was not done, and it is a different kind of job from
! the DaFile one. There the shim reproduced an existing mechanism; here it
! deletes one. RunFile keeps a fixed 1024-entry table of contents in the file,
! each entry a 16-character Label with a pointer, a length, a maximum length and
! a type. libspill's table of contents already is that, minus the fixed size,
! minus the pointer arithmetic, and minus the linear scan.
!
! The signatures below are OpenMolcas's, unchanged. gxWrRun, gxRdRun and ffxRun
! are the generic core; the 85 files above them -- dWrRun, Put_dArray,
! Get_iArray, qpg_*, and the rest -- call these and are untouched.
!
! Four things the port removes, each read out of gxwrrun.F90 and gxrdrun.F90
! rather than assumed:
!
! 1. THE FILE NEVER SHRINKS AND NEVER REUSES. When a record grows past its
!    MaxLen, gxWrRun marks the old slot Empty with Ptr = NulPtr and takes fresh
!    space at RunHdr%Next, which only ever increases. The abandoned bytes are
!    never reclaimed -- there is no free list anywhere in runfile_util. That is
!    the defect §7b disqualifies HDF5 for, sitting in a target code. libspill's
!    extent free list reclaims by construction. Someone has met this: gxwrrun.F90
!    still carries the commented-out warning "Label=... expands in RUNFILE with
!    size=" with a commented-out call to Abend beneath it.
!
! 2. nToc = 1024, FIXED. Overflow is "Ran out of ToC record in RunFile" followed
!    by Abend(). The same bug class as crayio's max_file (§6a) and DaFile's
!    MaxFileSize (§6c), a third time in the same corpus.
!
! 3. THE SCANS ARE LINEAR AND UNBOUNDED. Both the label lookup and the
!    free-slot search run the full 1024 entries with no early exit -- the
!    lookup keeps assigning `item` after it has found its match, and the
!    free-slot loop counts down from nToc without breaking. Every read and
!    every write pays for all of it. DIRAC's waio has the same defect, which
!    §6a records as one of the two things this library must do better.
!
! 4. EVERY CALL OPENS AND CLOSES THE FILE, and reads and rewrites the whole
!    table of contents around each transfer. A Put_dArray is: open, read 1024
!    entries, scan 1024, write data, write header, write 1024 entries, close.
!    The shim holds one store open for the run.
!
! What stays in OpenMolcas: the label whitelists (LabelsCA, LabelsDA and the
! rest) and the typed wrappers. Those are schema, and §3 is the reason they do
! not move here.

module runfile_ls
  use, intrinsic :: iso_c_binding
  use libspill
  implicit none
  public

  ! OpenMolcas's Definitions module supplies these; ISO_C_BINDING kinds stand in
  ! so the shim builds outside the tree. A 32-bit-integer build would set
  ! iwp = c_int32_t, which is why the item size is computed rather than assumed.
  integer, parameter :: iwp = c_int64_t, wp = c_double

  ! verbatim from runfile_data.F90
  integer, parameter :: lw = 16
  integer, parameter :: rcOK = 0, rcNotFound = 1, rcWrongType = 2
  integer, parameter :: TypUnk = 0, TypInt = 1, TypDbl = 2, TypStr = 3, TypLgl = 4

  type(c_ptr), save :: run_store = c_null_ptr
  logical, save     :: run_open = .false.
  character(len=64), save :: run_name = 'RUNFILE'
  character(kind=c_char), save, target :: run_dir(512) = c_null_char
  logical, save :: have_dir = .false.

contains

  ! Bytes per item for a record type. gzRWRun dispatches to iDaFile, dDaFile or
  ! cDaFile, so the item size is the Fortran kind's, not a constant.
  function item_bytes(RecTyp) result(n)
    integer, intent(in) :: RecTyp
    integer :: n
    select case (RecTyp)
      case (TypInt) ; n = storage_size(1_iwp) / 8
      case (TypDbl) ; n = storage_size(1.0_wp) / 8
      case (TypStr) ; n = 1
      case default  ; n = 0
    end select
  end function item_bytes

  ! A key is the label with trailing blanks removed. RunFile compares the full
  ! 16-character field, so two labels differing only in trailing blanks are the
  ! same record there and must stay the same record here.
  function key_of(Label) result(k)
    character(len=*), intent(in) :: Label
    character(len=lw) :: padded
    character(len=:), allocatable :: k
    padded = Label
    k = trim(padded)
    if (len(k) == 0) k = '_empty_'
  end function key_of

  subroutine run_ensure(iRc)
    integer, intent(out) :: iRc
    type(ls_opts_t) :: o
    integer :: err
    iRc = rcOK
    if (run_open) return
    o = ls_defaults()
    if (have_dir) o%dir = c_loc(run_dir(1))
    run_store = ls_open_f(trim(run_name), o, err)
    if (err /= LS_OK) then
      write(6,*) 'RunFile: cannot open ', trim(run_name), ': ', trim(ls_strerror_f(err))
      iRc = rcNotFound
      return
    end if
    run_open = .true.
  end subroutine run_ensure

  ! RunFile stores the item count and the type in its table of contents. Here
  ! they travel as the key's attribute blob (§3a(3)) -- eight opaque bytes the
  ! store never interprets, which is exactly the concession that section makes.
  subroutine meta_put(k, nData, RecTyp, iRc)
    character(len=*), intent(in) :: k
    integer, intent(in) :: nData, RecTyp
    integer, intent(out) :: iRc
    integer(c_int32_t), target :: meta(2)
    meta(1) = int(RecTyp, c_int32_t)
    meta(2) = int(nData, c_int32_t)
    iRc = ls_set_attr_f(run_store, k, c_loc(meta), 8)
  end subroutine meta_put

  subroutine meta_get(k, nData, RecTyp, found)
    character(len=*), intent(in) :: k
    integer, intent(out) :: nData, RecTyp
    logical, intent(out) :: found
    integer(c_int32_t), target :: meta(2)
    integer :: n, rc
    n = 8
    rc = ls_get_attr_f(run_store, k, c_loc(meta), n)
    found = (rc == LS_OK) .and. (n == 8)
    if (found) then
      RecTyp = int(meta(1))
      nData  = int(meta(2))
    else
      RecTyp = TypUnk
      nData  = 0
    end if
  end subroutine meta_get

end module runfile_ls

! ---------------------------------------------------------------------------

subroutine NameRun(fname)
  use runfile_ls
  implicit none
  character(len=*), intent(in) :: fname
  integer :: rc
  if (run_open) then
    rc = ls_close_f(run_store, 1)
    run_open = .false.
  end if
  run_name = fname
end subroutine NameRun

subroutine runfile_ls_set_dir(dir)
  use runfile_ls
  implicit none
  character(len=*), intent(in) :: dir
  integer :: i, n
  n = min(len_trim(dir), size(run_dir) - 1)
  do i = 1, n
    run_dir(i) = dir(i:i)
  end do
  run_dir(n+1) = c_null_char
  have_dir = (n > 0)
end subroutine runfile_ls_set_dir

! OpenMolcas opens and closes the RunFile around every single call. The shim
! keeps one store open, so it needs a point at which to let go; fin_run_use is
! already that point in the tree.
subroutine Fin_Run_Use
  use runfile_ls
  implicit none
  integer :: rc
  if (run_open) then
    rc = ls_close_f(run_store, 1)
    run_open = .false.
  end if
end subroutine Fin_Run_Use

subroutine gxWrRun(iRc, Label, cData, nData, iOpt, RecTyp)
  use runfile_ls
  implicit none
  integer, intent(out) :: iRc
  character(len=*), intent(in) :: Label
  character, intent(in) :: cData(*)
  integer, intent(in) :: nData, iOpt, RecTyp
  integer :: nbytes, rc

  call gxWrRun_Internal(cData)

contains

  subroutine gxWrRun_Internal(B)
    character, target, intent(in) :: B(*)
    character(len=:), allocatable :: k

    select case (RecTyp)
      case (TypInt, TypDbl, TypStr)
        ! ok
      case (TypLgl)
        call SysAbendMsg('gxWrRun', 'Records of logical type not implemented', 'Aborting')
      case default
        call SysAbendMsg('gxWrRun', 'Argument RecTyp is of wrong type', 'Aborting')
    end select
    if (nData < 0) call SysAbendMsg('gxWrRun', 'Number of data items less than zero', 'Aborting')
    if (iOpt /= 0) call SysAbendMsg('gxWrRun', 'Illegal option flag', ' ')

    call run_ensure(iRc)
    if (iRc /= rcOK) return

    k = key_of(Label)
    nbytes = nData * item_bytes(RecTyp)

    ! No slot to find, no MaxLen to outgrow, no Ptr to assign, and no abandoned
    ! extent when the record grows: libspill's free list takes the old space
    ! back. This is the whole of what gxWrRun's 90 lines were doing.
    if (nbytes > 0) then
      rc = ls_write_f(run_store, k, 0_c_int64_t, nbytes, c_loc(B(1)))
      if (rc /= LS_OK) then
        write(6,*) 'gxWrRun: ', trim(Label), ': ', trim(ls_strerror_f(rc))
        iRc = rcNotFound
        return
      end if
    end if
    call meta_put(k, nData, RecTyp, rc)
    iRc = rcOK
  end subroutine gxWrRun_Internal

end subroutine gxWrRun

subroutine gxRdRun(iRc, Label, cData, nData, iOpt, RecTyp)
  use runfile_ls
  implicit none
  integer, intent(out) :: iRc
  character(len=*), intent(in) :: Label
  character, intent(inout) :: cData(*)
  integer, intent(in) :: nData, iOpt, RecTyp
  integer :: rc

  call gxRdRun_Internal(cData)

contains

  subroutine gxRdRun_Internal(B)
    character, target, intent(inout) :: B(*)
    character(len=:), allocatable :: k
    integer :: haveN, haveTyp, nbytes
    logical :: found

    if (iOpt /= 0) call SysAbendMsg('gxRdRun', 'Illegal option flag', ' ')

    call run_ensure(iRc)
    if (iRc /= rcOK) return

    k = key_of(Label)
    call meta_get(k, haveN, haveTyp, found)
    if (.not. found) then
      call SysAbendMsg('gxRdRun', 'Record not found in runfile: '//trim(Label), ' ')
      iRc = rcNotFound
      return
    end if

    nbytes = nData * item_bytes(RecTyp)
    if (nbytes > 0) then
      rc = ls_read_f(run_store, k, 0_c_int64_t, nbytes, c_loc(B(1)))
      if (rc /= LS_OK) then
        write(6,*) 'gxRdRun: ', trim(Label), ': ', trim(ls_strerror_f(rc))
        iRc = rcNotFound
        return
      end if
    end if
    iRc = rcOK
  end subroutine gxRdRun_Internal

end subroutine gxRdRun

! The query. RunFile reopens the file and scans all 1024 entries to answer it;
! here it is one hash lookup of the attribute blob.
subroutine ffxRun(iRc, Label, nData, RecTyp, iOpt)
  use runfile_ls
  implicit none
  integer, intent(out) :: iRc, nData, RecTyp
  character(len=*), intent(in) :: Label
  integer, intent(in) :: iOpt
  logical :: found

  if (iOpt /= 0) call SysAbendMsg('ffxRun', 'Illegal option flag', ' ')

  nData = 0
  RecTyp = TypUnk
  call run_ensure(iRc)
  if (iRc /= rcOK) then
    iRc = rcNotFound
    return
  end if

  call meta_get(key_of(Label), nData, RecTyp, found)
  iRc = merge(rcOK, rcNotFound, found)
  if (.not. found) then
    nData = 0
    RecTyp = TypUnk
  end if
end subroutine ffxRun
