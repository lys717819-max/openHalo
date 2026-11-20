#ifndef HALO_PMC_H
#define HALO_PMC_H

#include <stdio.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <sys/wait.h>
#include "postgres.h"
#include "utils/hsearch.h"  // for HTAB

#include <sys/ioctl.h>
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <sched.h>      // CPU 亲和性类函数
#include <unistd.h>     /* sysconf */

/* 参数：要打开的 CPU PMC 记数器 */
extern char *halo_pmc_list;

inline unsigned long rdtscp()
{
#if defined(HW_aarch64)
    unsigned long tsc;
    asm volatile
    (
        "mrs %0, cntvct_el0":"=r"(tsc)
    );
    return tsc;
#else
    unsigned int    lo,
                    hi;
    __asm__ __volatile__
    (
        "rdtscp":"=a"(lo),"=d"(hi)::"rcx"
    );
    return (unsigned long)hi<<32|lo;
#endif
}

inline unsigned long rdtsc()
{
#if defined(HW_aarch64)
    unsigned long tsc;
    asm volatile
    (
        "mrs %0, cntvct_el0":"=r"(tsc)
    );
    return tsc;
#else
    unsigned int    lo,
                    hi;
    __asm__ __volatile__
    (
        "rdtsc":"=a"(lo),"=d"(hi)::"rcx"
    );
    return (unsigned long)hi<<32|lo;
#endif
}

/* PERF_FORMAT_GROUP + TIME_ENABLED + TIME_RUNNING
 * 每个文件描述符独立对应一个计数器，不包含 PERF_FORMAT_ID。
 */
struct read_format
{
    uint64 nr;
    uint64 time_enabled;
    uint64 time_running;

    struct
    {
        uint64 value;
    } values[];
};

#define PMC_DATA_SIZE 32
#define PMC_NUM 10          // CPU 中最多有 10 个记录结果的寄存器

extern MemoryContext TopMemoryContext;

struct pmc_data
{
    int fd;
    struct perf_event_mmap_page *mmapPC;
    size_t mmap_len;
    uint64 id;
    char name[40];
    uint32 pmutype;      // PMU Type, PERF_TYPE_HARDWARE, ...
    uint64 pe_config;    // PMU config, PERF_COUNT_HW_INSTRUCTIONS or umask & event, ...
    uint64 config1;
    uint64 config2;
    int pid;
    int num;
    unsigned long value;
    unsigned long time_enabled;
    unsigned long time_running;
    double scale;
    double scale_value;
    HTAB *pmcHashTab;
};

typedef struct PMCHashItem
{
    char pmcName[40];
    uint64 pmuType;
    uint64 peConfig;
    uint64 config1;
    uint64 config2;
} PMCHashItem;

extern struct pmc_data _PmcData[10];
// extern char __start_HALO_WATCH_PMC;
// extern char __stop_HALO_WATCH_PMC;

extern void SetPmcData(struct pmc_data *pmc, char *name, uint32 pmutype, uint64 pe_config, uint64  config1, uint64 config2, int pid);
extern void PmcOpenInternal(struct pmc_data *pmc);
extern void PmcClose(struct pmc_data *pmc);
extern void PmcInit(void);
extern int PmcOpen(void);
extern int SetFirstPmc(struct pmc_data *pmc);
extern void PmcReadData(uint32 cumulation);
extern bool PmcReadFast(int index, uint64 *value);
extern bool halo_pmc_fast;
extern bool PmcReadNormal(int index, uint64 *value);
extern bool PmcReadCounter(int index, uint64 *value);
extern void PmcPrint(void);

static inline long
perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
                int cpu, int group_fd, unsigned long flags)
{
    int ret;

    ret = syscall(__NR_perf_event_open, hw_event, pid, cpu,
                    group_fd, flags);
    return ret;
}

static inline void
PmcEnable()
{
    prctl(PR_TASK_PERF_EVENTS_ENABLE);
    return;
}

static inline void
PmcDisable()
{
    prctl(PR_TASK_PERF_EVENTS_DISABLE);
    return;
}

static inline void
PmcReset()
{
    int                 i,
                        num;
    struct pmc_data     *pmc;

    prctl(PR_TASK_PERF_EVENTS_DISABLE);

    pmc = _PmcData;
    num = pmc[0].num;
    for(i = 0; i < num; i++)
    {
        ioctl(pmc[i].fd, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
    }
}

#endif							/* HALO_PMC_H */
/* HALO CODES END */
