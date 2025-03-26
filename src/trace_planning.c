#include "postgres.h"
#include <time.h>
#include "utils/plancache.h"

#include "trace_lock_on_buffers.h"
#include "trace_session.h"
#include "trace_file.h"

#include "trace_planning.h"

typedef struct PlannerTrace
{
    uint64 startTime;
} PlannerTrace;


typedef struct GetCachedPlanTrace
{
    CachedPlanSource *plansource;
    ParamListInfo boundParams;
    int64 prevNumCustomPlans;
    int64 prevNumGenericPlans;
} GetCachedPlanTrace;


static PlannerTrace plannerTrace = { .startTime = 0};

static GetCachedPlanTrace getCachedPlanTrace = {.plansource = NULL,
                                                .prevNumCustomPlans = 0,
                                                .prevNumGenericPlans = 0};

static void PlannerTraceInFunc(void* data);
static void PlannerTraceRetFunc(void* data);
static void PlannerTraceCleanFunc(UprobeAttachInterface* uprobe);
static void GetCachedPlanInFunc(void* data, CachedPlanSource *plansource, ParamListInfo boundParams);
static void GetCachedPlanRetFunc(void* data);
static void GetCachedPlanCleanFunc(UprobeAttachInterface* uprobe);

static void
PlannerTraceInFunc(void* data)
{
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);

    plannerTrace.startTime = time.tv_sec * 1000000000L + time.tv_nsec;

    LockOnBuffersTraceStatPush();
}


static void
PlannerTraceRetFunc(void* data)
{
    struct timespec time;
    uint64 timeDiff;
    clock_gettime(CLOCK_MONOTONIC, &time);

    timeDiff = time.tv_sec * 1000000000L + time.tv_nsec - plannerTrace.startTime;
    if (writeMode == TEXT_WRITE_MODE)
        TracePrintf("TRACE PLAN. planningTime %lu nanosec\n", timeDiff);
    else
        TracePrintf("\"planningTime\": \"%lu nanosec\",\n", timeDiff);
    LockOnBuffersTraceDumpCurrentStatWithPrefix("Buffer locks usage for planning", "LWLockPlanning");
    LockOnBuffersTraceStatPop();
}


static void
PlannerTraceCleanFunc(UprobeAttachInterface* uprobe)
{
    pfree(uprobe);
}


static void
GetCachedPlanInFunc(void* data, CachedPlanSource *plansource, ParamListInfo boundParams)
{
    getCachedPlanTrace.plansource = plansource;
    getCachedPlanTrace.prevNumCustomPlans = plansource->num_custom_plans;
    getCachedPlanTrace.prevNumGenericPlans = plansource->num_generic_plans;
    getCachedPlanTrace.boundParams = boundParams;
}


static void
GetCachedPlanRetFunc(void* data)
{
    char* boundParamsLogStr;
    if (getCachedPlanTrace.boundParams)
        boundParamsLogStr = BuildParamLogString(getCachedPlanTrace.boundParams, NULL, -1);
    else
        boundParamsLogStr = "";

    if (getCachedPlanTrace.plansource->num_custom_plans != getCachedPlanTrace.prevNumCustomPlans)
    {
        if (writeMode == TEXT_WRITE_MODE)
            TracePrintf("TRACE GET_CACHED_PLAN. Custom plan was chosen for boundParams: %s", boundParamsLogStr);
        else
            TracePrintf("    \"planType\": \"custom\",\n    \"params\": \"%s\",\n", boundParamsLogStr);

    }
    else if (getCachedPlanTrace.plansource->num_generic_plans != getCachedPlanTrace.prevNumGenericPlans)
    {
        if (writeMode == TEXT_WRITE_MODE)
            TracePrintf("TRACE GET_CACHED_PLAN. Generic plan was chosen for boundParams: %s", boundParamsLogStr);
        else
            TracePrintf("    \"planType\": \"generic\",\n    \"params\": \"%s\",\n", boundParamsLogStr);
    }
    if (boundParamsLogStr[0] != '\0') //check on emty params str
        pfree(boundParamsLogStr);
}


static void
GetCachedPlanCleanFunc(UprobeAttachInterface* uprobe)
{
    pfree(uprobe);
}

//return 2 Uprobes to attach in resUrpobes array
void
PlanningUprobesGet(UprobeAttachInterface** resUprobes)
{
    resUprobes[0] = (UprobeAttachInterface*) palloc0(sizeof(UprobeAttachInterface));
    resUprobes[0]->cleanFunc = PlannerTraceCleanFunc;
    resUprobes[0]->inFunc = PlannerTraceInFunc;
    resUprobes[0]->retFunc = PlannerTraceRetFunc;
    resUprobes[0]->targetSymbol = "planner";

    resUprobes[1] = (UprobeAttachInterface*) palloc0(sizeof(UprobeAttachInterface));
    resUprobes[1]->cleanFunc = GetCachedPlanCleanFunc;
    resUprobes[1]->inFunc = GetCachedPlanInFunc;
    resUprobes[1]->retFunc = GetCachedPlanRetFunc;
    resUprobes[1]->numArgs = 2;
    resUprobes[1]->targetSymbol = "GetCachedPlan";
}