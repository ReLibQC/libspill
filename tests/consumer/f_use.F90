! SPDX-License-Identifier: BSD-3-Clause
program f_use
  use, intrinsic :: iso_c_binding
  use libspill
  implicit none
  type(ls_opts_t) :: o
  type(c_ptr) :: s
  integer(c_int) :: err, rc
  real(c_double), target :: v(4), b(4)
  v = [1.0d0, 2.0d0, 3.0d0, 4.0d0]; b = 0.0d0
  o = ls_defaults()
  s = ls_open_f('consumer_f', o, err)
  if (err /= LS_OK) stop 1
  rc = ls_write_f(s, 'k', 0_c_int64_t, int(32, c_size_t), c_loc(v))
  rc = ls_read_f (s, 'k', 0_c_int64_t, int(32, c_size_t), c_loc(b))
  rc = ls_close_f(s, 0_c_int)
  if (all(b == v)) then
    write(6,'(a)') '  [PASS] Fortran consumer via find_package'
  else
    write(6,'(a)') '  [FAIL] Fortran consumer via find_package'
    stop 1
  end if
end program f_use
