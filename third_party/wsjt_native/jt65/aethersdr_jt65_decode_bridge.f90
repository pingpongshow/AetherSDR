subroutine aethersdr_jt65_decode(c_symbols, c_erased, c_message, nerr, rc) bind(c, name="aethersdr_jt65_decode")
  use iso_c_binding, only: c_char, c_int, c_null_char
  use packjt, only: unpackmsg
  implicit none

  integer(c_int), intent(in) :: c_symbols(63)
  integer(c_int), intent(in) :: c_erased(63)
  character(kind=c_char), intent(out) :: c_message(23)
  integer(c_int), intent(out) :: nerr
  integer(c_int), intent(out) :: rc

  integer(c_int) :: recd(63)
  integer(c_int) :: erased(63)
  integer(c_int) :: era_pos(63)
  integer(c_int) :: decoded(12)
  integer(c_int) :: numera
  integer(c_int) :: iz
  integer :: i
  character(len=22) :: msg

  do i = 1, 63
     recd(i) = c_symbols(i)
     erased(i) = c_erased(i)
  end do

  call graycode65(recd, 63, -1)
  call interleave63(recd, -1)
  call interleave63(erased, -1)

  numera = 0
  do i = 1, 63
     if (erased(i) .ne. 0) then
        numera = numera + 1
        era_pos(numera) = i - 1
     end if
  end do

  call rs_decode(recd, era_pos, numera, decoded, nerr)
  if (nerr .lt. 0) then
     rc = 1
     do i = 1, 22
        c_message(i) = c_null_char
     end do
     c_message(23) = c_null_char
     return
  end if

  msg = '                      '
  call unpackmsg(decoded, msg)
  call fmtmsg(msg, iz)

  do i = 1, 22
     c_message(i) = msg(i:i)
  end do
  c_message(23) = c_null_char
  rc = 0
end subroutine aethersdr_jt65_decode
