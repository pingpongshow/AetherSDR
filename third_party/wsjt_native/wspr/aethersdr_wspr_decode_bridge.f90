subroutine aethersdr_wspr_decode(c_soft_symbols, c_message, metric, ncycles, rc) bind(c, name="aethersdr_wspr_decode")
  use iso_c_binding, only: c_char, c_int, c_null_char
  implicit none

  integer(c_int), intent(in) :: c_soft_symbols(162)
  character(kind=c_char), intent(out) :: c_message(23)
  integer(c_int), intent(out) :: metric
  integer(c_int), intent(out) :: ncycles
  integer(c_int), intent(out) :: rc

  integer*1 softsym(0:161)
  integer*1 data0(11)
  integer mettab(0:255,0:1)
  integer(c_int) :: local_metric
  integer(c_int) :: local_cycles
  integer :: i
  integer :: ierr
  integer :: nbits
  integer :: ndelta
  integer :: limit
  integer :: ntype
  character(len=22) :: message

  do i = 1, 162
     softsym(i - 1) = int(max(0_c_int, min(255_c_int, c_soft_symbols(i))), kind=1)
  end do

  call inter_wspr(softsym, -1)

  do i = 0, 255
     mettab(i,0) = 255 - i
     mettab(i,1) = i
  end do

  nbits = 81
  ndelta = 60
  limit = 10000
  local_metric = 0
  local_cycles = 0
  message = '                      '

  call fano232(softsym, nbits, mettab, ndelta, limit, data0, local_cycles, local_metric, ierr)
  if (ierr .ne. 0) then
     metric = local_metric
     ncycles = local_cycles
     rc = 1
     do i = 1, 22
        c_message(i) = c_null_char
     end do
     c_message(23) = c_null_char
     return
  end if

  call wqdecode(data0, message, ntype)
  do i = 1, 22
     c_message(i) = message(i:i)
  end do
  c_message(23) = c_null_char
  metric = local_metric
  ncycles = local_cycles
  rc = 0
end subroutine aethersdr_wspr_decode
