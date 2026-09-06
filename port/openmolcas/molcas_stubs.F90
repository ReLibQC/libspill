! Routines OpenMolcas supplies that the shims call on fatal argument errors.
! In OpenMolcas these already exist -- SysAbendMsg prints a formatted banner and
! calls Abend() -- so this file is deleted on the way in. It is here only so the
! shims link outside the tree.
subroutine SysAbendMsg(loc, t1, t2)
  implicit none
  character(len=*), intent(in) :: loc, t1, t2
  write(6,*) 'SysAbendMsg: ', trim(loc), ': ', trim(t1), ' ', trim(t2)
  error stop 1
end subroutine SysAbendMsg
