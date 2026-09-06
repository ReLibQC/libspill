! SPDX-License-Identifier: BSD-3-Clause
! A caller built -fdefault-integer-8, against a libspill module built WITHOUT
! it. eT and OpenMolcas both default to -i8, so this combination is the normal
! one for this library's audience, not an exotic one.
!
! Issue #3: the binding's logical dummies had no explicit kind, and
! -fdefault-integer-8 widens LOGICAL as well as INTEGER, so ls_exists_f,
! ls_test_f and ls_store_exists_f could not be called at all. Compiling this
! file is the check; it is never run.
program i8_caller
   use, intrinsic :: iso_c_binding
   use libspill
   implicit none
   type(ls_opts_t) :: o
   type(c_ptr) :: s, req, addr
   integer(c_int) :: rc, err
   integer(c_int64_t) :: n64, off
   integer(c_size_t) :: an
   logical(c_bool) :: found, done
   real(c_double), target :: buf(8)

   ! Every entry point, with arguments declared the way an -i8 caller declares
   ! them. A dummy whose kind depends on either side's flags fails here.
   o = ls_defaults()
   s = ls_open_f('i8store', o, err)
   rc = ls_write_f(s, 'k', 0_c_int64_t, int(64, c_size_t), c_loc(buf))
   rc = ls_read_f (s, 'k', 0_c_int64_t, int(64, c_size_t), c_loc(buf))
   rc = ls_append_f(s, 'k', int(64, c_size_t), c_loc(buf), off)
   rc = ls_reserve_f(s, 'k', 64_c_int64_t)
   rc = ls_exists_f(s, 'k', found)
   rc = ls_size_f(s, 'k', n64)
   rc = ls_erase_f(s, 'k')
   rc = ls_set_attr_f(s, 'k', c_loc(buf), 8_c_size_t)
   an = 8
   rc = ls_get_attr_f(s, 'k', c_loc(buf), an)
   rc = ls_aread_f(s, 'k', 0_c_int64_t, int(64, c_size_t), c_loc(buf), req)
   rc = ls_test_f(req, done)
   rc = ls_wait_f(req)
   rc = ls_map_f(s, 'k', addr, an)
   rc = ls_unmap_f(s, 'k')
   rc = ls_unlink_now_f(s)
   rc = ls_store_exists_f('i8store', o, found)
   rc = ls_close_f(s, 0_c_int)
   if (.not. ls_abi_ok()) error stop 1
   write(6,'(a)') '  [PASS] every entry point is callable from an -i8 build'
end program i8_caller
