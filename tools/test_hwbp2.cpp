#include <cstdio>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <linux/elf.h>
#include "mem_core.hpp"

#ifndef NT_ARM_HW_BREAK
#define NT_ARM_HW_BREAK 0x402
#endif
#ifndef NT_ARM_HW_WATCH
#define NT_ARM_HW_WATCH 0x403
#endif
#ifndef PTRACE_SEIZE
#define PTRACE_SEIZE 0x4206
#endif
#ifndef PTRACE_INTERRUPT
#define PTRACE_INTERRUPT 0x4207
#endif

static long peo(struct perf_event_attr* a, pid_t p){
  return syscall(__NR_perf_event_open, a, p, -1, -1, 0);
}

int main(int c,char**v){
  int pid=atoi(v[1]);
  if(!mem::attach(pid)){printf("attach fail\n");return 1;}
  auto maps=mem::load_maps(true);
  uintptr_t a=0;
  for(auto&r:maps){ if(r.writable&&r.end-r.start>0x1000&&r.start>0x10000){a=r.start+0x200;break;}}
  printf("addr=%llx\n",(unsigned long long)a);

  // try many perf configs
  int configs[][6]={
    // bp_type, bp_len, sample_period, precise, inherit, disabled
    {HW_BREAKPOINT_W, HW_BREAKPOINT_LEN_4, 1, 0, 0, 0},
    {HW_BREAKPOINT_W, HW_BREAKPOINT_LEN_4, 0, 0, 0, 0},
    {HW_BREAKPOINT_W, HW_BREAKPOINT_LEN_8, 1, 0, 0, 0},
    {HW_BREAKPOINT_W, HW_BREAKPOINT_LEN_4, 1, 0, 1, 0},
    {HW_BREAKPOINT_RW, HW_BREAKPOINT_LEN_4, 1, 0, 0, 0},
    {HW_BREAKPOINT_X, HW_BREAKPOINT_LEN_4, 1, 0, 0, 0},
  };
  for(int i=0;i<6;i++){
    struct perf_event_attr attr{};
    attr.type=PERF_TYPE_BREAKPOINT;
    attr.size=sizeof(attr);
    attr.bp_type=configs[i][0];
    attr.bp_addr=a & ~3ull;
    attr.bp_len=configs[i][1];
    attr.sample_period=configs[i][2];
    if(configs[i][2]) attr.sample_type=PERF_SAMPLE_IP;
    attr.precise_ip=configs[i][3];
    attr.inherit=configs[i][4];
    attr.disabled=configs[i][5];
    attr.exclude_kernel=1;
    attr.exclude_hv=1;
    errno=0;
    long fd=peo(&attr, pid);
    printf("perf cfg%d type=%d len=%d per=%d inh=%d -> fd=%ld errno=%d %s\n",
      i, configs[i][0], configs[i][1], configs[i][2], configs[i][4],
      fd, errno, strerror(errno));
    if(fd>=0){ close(fd); break; }
  }
  // self process test
  {
    struct perf_event_attr attr{};
    attr.type=PERF_TYPE_BREAKPOINT; attr.size=sizeof(attr);
    attr.bp_type=HW_BREAKPOINT_W; attr.bp_addr=(uint64_t)&a; attr.bp_len=HW_BREAKPOINT_LEN_8;
    attr.exclude_kernel=1; attr.disabled=0;
    errno=0; long fd=peo(&attr, 0);
    printf("perf self pid=0 -> fd=%ld errno=%d %s\n", fd, errno, strerror(errno));
    if(fd>=0) close(fd);
  }

  // ptrace HW
  errno=0;
  long r=ptrace(PTRACE_SEIZE, pid, 0, 0);
  printf("SEIZE %ld errno=%d %s\n", r, errno, strerror(errno));
  if(r==0){
    ptrace(PTRACE_INTERRUPT, pid, 0, 0);
    int st=0; waitpid(pid,&st,0);
    printf("stopped sig=%d\n", WSTOPSIG(st));
    struct user_hwdebug_state {
      uint32_t dbg_info; uint32_t pad;
      struct { uint64_t addr; uint32_t ctrl; uint32_t pad; } dbg_regs[16];
    } br{}, wa{};
    // get first
    iovec iv{&br, sizeof(br)};
    errno=0;
    long g=ptrace(PTRACE_GETREGSET, pid, (void*)(uintptr_t)NT_ARM_HW_BREAK, &iv);
    printf("GETREGSET BREAK %ld errno=%d %s iov_len=%zu dbg_info=0x%x\n",
      g, errno, strerror(errno), iv.iov_len, br.dbg_info);
    int nslots = br.dbg_info & 0xff;
    printf("  break slots=%d\n", nslots);
    iv.iov_base=&wa; iv.iov_len=sizeof(wa);
    errno=0;
    g=ptrace(PTRACE_GETREGSET, pid, (void*)(uintptr_t)NT_ARM_HW_WATCH, &iv);
    printf("GETREGSET WATCH %ld errno=%d %s dbg_info=0x%x slots=%d\n",
      g, errno, strerror(errno), wa.dbg_info, wa.dbg_info&0xff);

    // set one break
    memset(&br,0,sizeof(br));
    br.dbg_regs[0].addr = a & ~3ull;
    br.dbg_regs[0].ctrl = 0x1 | (2u<<1) | (0xFu<<5);
    iv.iov_base=&br; iv.iov_len=sizeof(br);
    errno=0;
    long s=ptrace(PTRACE_SETREGSET, pid, (void*)(uintptr_t)NT_ARM_HW_BREAK, &iv);
    printf("SETREGSET BREAK full %ld errno=%d %s\n", s, errno, strerror(errno));
    // try smaller iov - only 1 slot
    size_t one = 8 + 16; // dbg_info+pad + 1 reg
    // actually struct is 8 + 16*16 = 264
    // kernel may want exact size based on slots
    int slots = 6;
    size_t sz = 8 + slots * 16;
    char buf[512]{};
    memcpy(buf, &br, sizeof(br));
    iv.iov_base=buf; iv.iov_len=sz;
    errno=0;
    s=ptrace(PTRACE_SETREGSET, pid, (void*)(uintptr_t)NT_ARM_HW_BREAK, &iv);
    printf("SETREGSET BREAK sz=%zu %ld errno=%d %s\n", sz, s, errno, strerror(errno));

    // watch
    memset(&wa,0,sizeof(wa));
    wa.dbg_regs[0].addr = a & ~7ull;
    // enable | PMC=EL0 | LSC=store | BAS=0x0F (4 bytes)
    wa.dbg_regs[0].ctrl = 0x1 | (2u<<1) | (0x2u<<3) | (0x0Fu<<5);
    iv.iov_base=&wa; iv.iov_len=sizeof(wa);
    errno=0;
    s=ptrace(PTRACE_SETREGSET, pid, (void*)(uintptr_t)NT_ARM_HW_WATCH, &iv);
    printf("SETREGSET WATCH full %ld errno=%d %s\n", s, errno, strerror(errno));
    sz = 8 + 6*16;
    memcpy(buf,&wa,sizeof(wa));
    iv.iov_base=buf; iv.iov_len=sz;
    errno=0;
    s=ptrace(PTRACE_SETREGSET, pid, (void*)(uintptr_t)NT_ARM_HW_WATCH, &iv);
    printf("SETREGSET WATCH sz=%zu %ld errno=%d %s\n", sz, s, errno, strerror(errno));

    ptrace(PTRACE_DETACH, pid, 0, 0);
  }
  mem::detach();
  return 0;
}
