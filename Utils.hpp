#include <sched.h>
#include <errno.h>
#include <unistd.h>

namespace Utils
{
  inline uint64_t rdtscp( )
  {
      uint32_t  aux;
      uint64_t rax,rdx;
      asm volatile ( "rdtscp\n" : "=a" (rax), "=d" (rdx), "=c" (aux) : : );
      return (rdx << 32) + rax;
  }

  void pinThreadToCore(size_t coreId)
  {
    const pid_t pid = getpid();
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(coreId, &cpuset);
    sched_setaffinity(pid, sizeof(cpu_set_t), &cpuset);
  }

}
