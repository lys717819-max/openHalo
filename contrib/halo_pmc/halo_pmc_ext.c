#include "postgres.h"
#include "fmgr.h"
#include "utils/halo_pmc.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(halo_pmc_init);

Datum
halo_pmc_init(PG_FUNCTION_ARGS)
{
    PmcInit();

    PG_RETURN_BOOL(true);
}