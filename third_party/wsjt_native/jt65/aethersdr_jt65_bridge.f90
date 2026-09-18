subroutine aethersdr_jt65_encode(c_message, tones, c_msgsent, itype, rc) bind(c, name="aethersdr_jt65_encode")
  use iso_c_binding, only: c_char, c_int, c_null_char
  implicit none

  interface
     subroutine gen65(msg00, ichk, msgsent0, itone, local_itype) bind(c)
       use iso_c_binding, only: c_char, c_int
       character(kind=c_char) :: msg00(23)
       integer(c_int) :: ichk
       character(kind=c_char) :: msgsent0(23)
       integer(c_int) :: itone(126)
       integer(c_int) :: local_itype
     end subroutine gen65
  end interface

  character(kind=c_char), intent(in) :: c_message(23)
  integer(c_int), intent(out) :: tones(126)
  character(kind=c_char), intent(out) :: c_msgsent(23)
  integer(c_int), intent(out) :: itype
  integer(c_int), intent(out) :: rc

  character(kind=c_char) :: msg_in(23)
  character(kind=c_char) :: msg_out(23)
  integer(c_int) :: local_tones(126)
  integer(c_int) :: ichk
  integer(c_int) :: local_itype
  integer :: i

  do i = 1, 23
     msg_in(i) = c_null_char
     msg_out(i) = c_null_char
  end do

  do i = 1, 22
     if (c_message(i) == c_null_char) exit
     msg_in(i) = c_message(i)
  end do
  msg_in(23) = c_null_char

  ichk = 0
  local_tones = 0
  local_itype = 0

  call gen65(msg_in, ichk, msg_out, local_tones, local_itype)

  do i = 1, 126
     tones(i) = local_tones(i)
  end do

  do i = 1, 22
     c_msgsent(i) = msg_out(i)
  end do
  c_msgsent(23) = c_null_char

  itype = local_itype
  rc = 0
end subroutine aethersdr_jt65_encode
