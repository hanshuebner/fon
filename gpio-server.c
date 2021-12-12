#include <sys/socket.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <signal.h>
#include <sched.h>

void
disable_swapping()
{
  struct sched_param sp;
  memset(&sp, 0, sizeof(sp));
  sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
  sched_setscheduler(0, SCHED_FIFO, &sp);
  mlockall(MCL_CURRENT | MCL_FUTURE);
}

main()
{
  disable_swapping();
