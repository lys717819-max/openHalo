#include "postgres.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/varlena.h"
#include "executor/spi.h"
#include "commands/explain.h"
#include "utils/halo_pmc.h"
#include "access/xact.h"

char *halo_pmc_list = NULL;
bool halo_pmc_fast = true;

struct pmc_data __attribute__((section("HALO_PMC"))) _PmcData[10];

void
SetPmcData(struct pmc_data *pmc, char *name, uint32 pmutype, uint64 pe_config, uint64 config1, uint64 config2, int pid)
{
    struct pmc_data         *pd;

    pd = pmc + pmc->num;
    strncpy(pd->name, name, 40);
    pd->pmutype = pmutype;
    pd->pe_config = pe_config;
    pd->config1 = config1;
    pd->config2 = config2;
    pd->pid = pid;
    pd->num = pmc->num;
    pmc->num++;
}

void
PmcOpenInternal(struct pmc_data *pmc)
{
    int i;
    int num = pmc[0].num;

    PG_TRY();
    {
        for (i = 0; i < num; i++)
        {
            pmc[i].fd = SetFirstPmc(pmc + i);
            pmc[i].mmap_len = (size_t) sysconf(_SC_PAGESIZE);

pmc[i].mmapPC = mmap(NULL,
                     pmc[i].mmap_len,
                     PROT_READ,
                     MAP_SHARED,
                     pmc[i].fd,
                     0);

if (pmc[i].mmapPC == MAP_FAILED)
{
    pmc[i].mmapPC = NULL;
    pmc[i].mmap_len = 0;

    ereport(ERROR,
            (errmsg("Failed to mmap PMC event \"%s\": %m",
                    pmc[i].name)));
}

            if (ioctl(pmc[i].fd,
                      PERF_EVENT_IOC_RESET,
                      PERF_IOC_FLAG_GROUP) == -1)
            {
                ereport(ERROR,
                        (errmsg("Failed to reset PMC event \"%s\": %m",
                                pmc[i].name)));
            }
        }

        if (prctl(PR_TASK_PERF_EVENTS_ENABLE) == -1)
        {
            ereport(ERROR,
                    (errmsg("Failed to enable PMC counters: %m")));
        }
    }
    PG_CATCH();
    {
        PmcClose(pmc);
        PG_RE_THROW();
    }
    PG_END_TRY();
}

void
PmcClose(struct pmc_data *pmc)
{
    int k;
    int num = pmc[0].num;

    prctl(PR_TASK_PERF_EVENTS_DISABLE);

    for (k = 0; k < num; k++)
    {
        if (pmc[k].mmapPC != NULL)
        {
            munmap(pmc[k].mmapPC, pmc[k].mmap_len);
            pmc[k].mmapPC = NULL;
            pmc[k].mmap_len = 0;
        }

        if (pmc[k].fd >= 0)
        {
            close(pmc[k].fd);
            pmc[k].fd = -1;
        }
    }
}

// 初始化，使用前只需执行一次
void
PmcInit(void)
{
    uint32           i;
    HTAB            *hTable;
    HASHCTL          hctl;
    PMCHashItem     *pmcHashItem;
    MemoryContext    cxt;
    MemoryContext    old_cxt;
    struct pmc_data *pmc = _PmcData;
    const char      *sql =
        "SELECT lower(pcm_name), pmutype, pe_config, config1, config2 "
        "FROM public.pmc_name";

    if (pmc->pmcHashTab != NULL)
        return;

    cxt = AllocSetContextCreate(
        TopMemoryContext,
        "Halo PMC hash memctx",
        ALLOCSET_DEFAULT_SIZES);

    old_cxt = CurrentMemoryContext;

    PG_TRY();
    {
        MemoryContextSwitchTo(cxt);

        MemSet(&hctl, 0, sizeof(hctl));
        hctl.keysize = 40;
        hctl.entrysize = sizeof(PMCHashItem);
        hctl.hcxt = cxt;

        hTable = hash_create(
            "Halo PMC Hash",
            1024,
            &hctl,
            HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

        if (SPI_connect() != SPI_OK_CONNECT)
            elog(ERROR, "PMC SPI_connect failed");

        if (SPI_exec(sql, 0) != SPI_OK_SELECT)
            elog(ERROR, "PMC SPI_exec failed");

        for (i = 0; i < SPI_processed; i++)
        {
            char        key[40] = {0};
            TupleDesc   tupdesc = SPI_tuptable->tupdesc;
            HeapTuple   tuple = SPI_tuptable->vals[i];
            char       *name;
            bool        isNull;
            bool        found;
            Datum       value;

            name = SPI_getvalue(tuple, tupdesc, 1);

            if (name == NULL)
                elog(ERROR, "PMC name cannot be NULL");

            if (strlen(name) >= sizeof(key))
                elog(ERROR, "PMC name is too long: %s", name);

            strlcpy(key, name, sizeof(key));
            pfree(name);

            pmcHashItem = (PMCHashItem *)
                hash_search(hTable, key, HASH_ENTER, &found);

            value = SPI_getbinval(tuple, tupdesc, 2, &isNull);
            if (isNull)
                elog(ERROR, "PMC pmutype cannot be NULL");
            pmcHashItem->pmuType =
                (uint64) (uint32) DatumGetInt32(value);

            value = SPI_getbinval(tuple, tupdesc, 3, &isNull);
            if (isNull)
                elog(ERROR, "PMC pe_config cannot be NULL");
            pmcHashItem->peConfig =
                (uint64) (uint32) DatumGetInt32(value);

            value = SPI_getbinval(tuple, tupdesc, 4, &isNull);
            if (isNull)
                elog(ERROR, "PMC config1 cannot be NULL");
            pmcHashItem->config1 =
                (uint64) (uint32) DatumGetInt32(value);

            value = SPI_getbinval(tuple, tupdesc, 5, &isNull);
            if (isNull)
                elog(ERROR, "PMC config2 cannot be NULL");
            pmcHashItem->config2 =
                (uint64) (uint32) DatumGetInt32(value);
        }

        if (SPI_finish() != SPI_OK_FINISH)
            elog(ERROR, "PMC SPI_finish failed");

        MemoryContextSwitchTo(old_cxt);
        pmc->pmcHashTab = hTable;
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(old_cxt);
        MemoryContextDelete(cxt);
        PG_RE_THROW();
    }
    PG_END_TRY();
}

// PMC 的入口函数
int
PmcOpen(void)
{
    int                 i,
                        pid,
                        strLen;
    char                *rawStr;
    bool                found;
    List                *elemlist;
    ListCell            *l;
    PMCHashItem         *pmcHashItem;
    struct pmc_data     *pmc = _PmcData;
    HTAB                *hTable = _PmcData[0].pmcHashTab;
    if (hTable == NULL)
        ereport(ERROR,
                (errmsg("PMC is not initialized; call PmcInit first")));

    if (halo_pmc_list == NULL || halo_pmc_list[0] == '\0')
        return 0;
    /* Get PMC list */
    rawStr = pstrdup(halo_pmc_list);

    /* 转为小写字母 */
    strLen = strlen(rawStr);

    for (i = 0; i < strLen; i++)
    {
        rawStr[i] = pg_ascii_tolower(rawStr[i]);
    }

    /* Parse string into list of PMC */
    if (!SplitGUCList(rawStr, ',', &elemlist))
    {
        // syntax error in list
        ereport(ERROR,
        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
         errmsg("invalid list syntax in parameter \"%s\"",
                "halo_pmc_list")));
    }

    pid = 0;
    pmc->num = 0;
/* 初始化所有 PMC 文件描述符 */
for (i = 0; i < PMC_NUM; i++)
{
    pmc[i].fd = -1;
    pmc[i].mmapPC = NULL;
    pmc[i].mmap_len = 0;
}
    // memset(pmc, 0, sizeof(struct pmc_data) * PMC_NUM);

    foreach(l, elemlist)
    {
        char    hashKey[40]={0};
        char    *pmcName = (char *) lfirst(l);

        strncpy(hashKey, pmcName, 40);
        hashKey[39] = '\0';

        pmcHashItem = (PMCHashItem *) hash_search(hTable, hashKey, HASH_FIND, &found);
        if (!found)
        {
            ereport(ERROR,
            (errmsg("PMC event \"%s\" not found in pmc_name table",
                    pmcName)));
        }

        if (_PmcData[0].num >= PMC_NUM)
{
    ereport(ERROR,
            (errmsg("Too many PMC events, maximum is %d", PMC_NUM)));
}

SetPmcData(_PmcData, pmcName,
           pmcHashItem->pmuType,
           pmcHashItem->peConfig,
           pmcHashItem->config1,
           pmcHashItem->config2,
           pid);
    }

    // SetPmcData(_PmcData, "PERF_COUNT_HW_CACHE_L1D_RD", PERF_TYPE_HW_CACHE, PERF_COUNT_HW_CACHE_L1D|(PERF_COUNT_HW_CACHE_OP_READ<<8)|(PERF_COUNT_HW_CACHE_RESULT_ACCESS<<16), 0, 1, pid);
    // SetPmcData(_PmcData, "PERF_COUNT_HW_CACHE_L1D_MISS_RD", PERF_TYPE_HW_CACHE, PERF_COUNT_HW_CACHE_L1D|(PERF_COUNT_HW_CACHE_OP_READ<<8)|(PERF_COUNT_HW_CACHE_RESULT_MISS<<16), 0, 1, pid);
    // SetPmcData(_PmcData, "MEM_INST_RETIRED.ALL_STORES", PERF_TYPE_RAW, 0x82d0, 0, 1, pid);

    PmcOpenInternal(_PmcData);

    return _PmcData[0].num;
}

int
SetFirstPmc(struct pmc_data *pmc)
{
    struct perf_event_attr              pe;     // Linux 系统结构，linux/perf_event.h

    memset(&pe, 0, sizeof(struct perf_event_attr));
    pe.size = sizeof(struct perf_event_attr);
    pe.type = pmc->pmutype;
    pe.config = pmc->pe_config;
    pe.disabled = 1;
    pe.exclude_kernel = pmc->config2;
    pe.exclude_hv = pmc->config2;
    pe.inherit = 0;                     // diting 中为 1
    pe.config1 = pmc->config1;
    pe.config2 = 0;
    //pe.exclude_guest = 0;
    //pe.read_format = 3; //3 PERF_FORMAT_GROUP | PERF_FORMAT_ID;
    pe.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED|PERF_FORMAT_TOTAL_TIME_RUNNING|PERF_FORMAT_GROUP;
    pe.sample_type = 0;

    if ( pmc->config1 != 0x0614)
        pmc->fd = perf_event_open(&pe, pmc->pid, -1, -1, 0);
    else
        pmc->fd = perf_event_open(&pe, -1, pmc->pid, -1, 0);

    /*
        pmc->config1 == 0x6014 时，需要为 perf_event_open 指定 CPU 号，才能精确的统计内存控制器中 Memory 方面的数据
        pmc->pid 中不是进程号，而是 CPU 号。
    */

    if (pmc->fd == -1)
{
    ereport(ERROR,
            (errmsg("Failed to open PMC event \"%s\" (config=%llu): %m",
                    pmc->name,
                    (unsigned long long) pe.config)));
}
    ioctl( pmc->fd, PERF_EVENT_IOC_ID, &(pmc->id) );

    return pmc->fd;
}


    /* 使用 RDPMC 指令读取硬件性能计数器 */
#if defined(__x86_64__) || defined(__i386__)
static inline uint64
read_rdpmc(uint32 counter)
{
    uint32 lo, hi;

    __asm__ __volatile__(
        "lfence\n\t"
        "rdpmc\n\t"
        "lfence"
        : "=a"(lo), "=d"(hi)
        : "c"(counter)
        : "memory"
    );

    return ((uint64) hi << 32) | lo;
}
#endif

/* 低消耗读取指定 PMC 计数器 */
bool
PmcReadFast(int index, uint64 *value)
{
#if defined(__x86_64__) || defined(__i386__)
    volatile struct perf_event_mmap_page *pc;
    uint32 seq;
    uint32 pmc_index;
    uint16 width;
    uint64 count;
    uint64 pmc;
    bool supported;
    int retry;

    if (index < 0 || index >= _PmcData[0].num ||
        value == NULL)
        return false;

    pc = _PmcData[index].mmapPC;

    if (pc == NULL)
        return false;

    for (retry = 0; retry < 100; retry++)
    {
        seq = pc->lock;

        if (seq & 1)
            continue;

        __asm__ __volatile__("" ::: "memory");

        pmc_index = pc->index;
        width = pc->pmc_width;
        count = pc->offset;

        supported = pc->cap_user_rdpmc &&
                    pmc_index != 0 &&
                    width > 0 && width <= 64;

        if (supported)
        {
            pmc = read_rdpmc(pmc_index - 1);

            pmc <<= (64 - width);
            pmc = (uint64)
                  ((int64) pmc >> (64 - width));

            count += pmc;
        }

        __asm__ __volatile__("" ::: "memory");

        if (pc->lock == seq)
        {
            if (!supported)
                return false;

            *value = count;
            return true;
        }
    }

    /* 超过最大重试次数，返回读取失败 */
    return false;
#else
    return false;
#endif
}
/* 使用传统 read 系统调用读取计数器 */
bool
PmcReadNormal(int index, uint64 *value)
{
    uint64 buf[4];
    ssize_t bytes_read;

    if (index < 0 || index >= _PmcData[0].num ||
        value == NULL)
        return false;

    if (_PmcData[index].fd < 0)
        return false;

    bytes_read = read(_PmcData[index].fd,
                      buf, sizeof(buf));

    if (bytes_read != sizeof(buf) || buf[0] != 1)
        return false;

    *value = buf[3];

    return true;
}
/* 统一 PMC 读取接口 */
bool
PmcReadCounter(int index, uint64 *value)
{
    if (halo_pmc_fast)
        return PmcReadFast(index, value);

    return PmcReadNormal(index, value);
}
void
PmcReadData(uint32 cumulation)
{
    int                 num,
                        k;
    char                buf[PMC_DATA_SIZE];
    struct read_format* rf;
    struct pmc_data     *pmc;

    prctl(PR_TASK_PERF_EVENTS_DISABLE);

    pmc = _PmcData;
    rf = (struct read_format*)buf;

    // 得到打开的 PMC 数量
    num = pmc[0].num;

    for (k = 0; k < num; k++)
    {
        ssize_t bytes_read;

bytes_read = read(pmc[k].fd, rf, sizeof(buf));

if (pmc[k].mmapPC != NULL)
{
    munmap(pmc[k].mmapPC, pmc[k].mmap_len);
    pmc[k].mmapPC = NULL;
    pmc[k].mmap_len = 0;
}

close(pmc[k].fd);
pmc[k].fd = -1;

if (bytes_read != PMC_DATA_SIZE || rf->nr != 1)
{
    ereport(ERROR,
            (errmsg("Failed to read PMC event \"%s\": invalid counter data",
                    pmc[k].name)));
}

        if ( !cumulation )
        {
            pmc[k].value = rf->values[0].value;
            pmc[k].time_enabled = rf->time_enabled;
            pmc[k].time_running = rf->time_running;
            if (rf->time_enabled == 0 || rf->time_running == 0)
{
    pmc[k].scale = 0.0;
    pmc[k].scale_value = 0.0;
}
else
{
    pmc[k].scale =
        (double) rf->time_running / rf->time_enabled;

    pmc[k].scale_value =
        (double) rf->values[0].value / pmc[k].scale;
}
        }
        else
{
    double t = 0.0;

    pmc[k].value += rf->values[0].value;
    pmc[k].time_enabled += rf->time_enabled;
    pmc[k].time_running += rf->time_running;

    if (rf->time_enabled > 0 && rf->time_running > 0)
    {
        t = (double) rf->time_running / rf->time_enabled;

        pmc[k].scale += t;
        pmc[k].scale_value +=
            (double) rf->values[0].value / t;
    }
}
    }
    return ;
}

void
PmcPrint(void)
{
    int                 i,
                        num,
                        cur_cpu;
    struct pmc_data     *pmc = _PmcData;
    char                str[200];

    num = pmc[0].num;
    cur_cpu = sched_getcpu();
    snprintf(str, 10, "CPU:%d", cur_cpu);
    elog(INFO, "%s", str);

    for(i = 0; i < num; i++)
    {
		snprintf(str, 150, "%30s : %8.2f (%.2f) %8ld",
                                    pmc[i].name,
                                    pmc[i].scale_value,
                                    pmc[i].scale * 100,
                                    pmc[i].value);
        elog(INFO, "%s", str);
    }
}
