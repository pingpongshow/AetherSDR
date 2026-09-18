subroutine aethersdr_wspr_encode(c_message, tones, c_msgsent, rc) bind(c, name="aethersdr_wspr_encode")
  use iso_c_binding, only: c_char, c_int, c_null_char
  implicit none

  character(kind=c_char), intent(in) :: c_message(23)
  integer(c_int), intent(out) :: tones(162)
  character(kind=c_char), intent(out) :: c_msgsent(23)
  integer(c_int), intent(out) :: rc

  character(len=22) :: message
  character(len=22) :: msgsent
  integer(c_int) :: local_tones(162)
  integer :: i

  message = ' '
  msgsent = ' '
  local_tones = 0

  do i = 1, 22
     if (c_message(i) == c_null_char) exit
     message(i:i) = c_message(i)
  end do

  call genwspr(message, msgsent, local_tones)

  do i = 1, 162
     tones(i) = local_tones(i)
  end do

  do i = 1, 22
     c_msgsent(i) = msgsent(i:i)
  end do
  c_msgsent(23) = c_null_char

  rc = 0
end subroutine aethersdr_wspr_encode
