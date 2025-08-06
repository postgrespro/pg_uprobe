#include "postgres.h"
#include <time.h>
#include "utils/plancache.h"

#include "trace_lock_on_buffers.h"
#include "trace_session.h"
#include "trace_file.h"

#include "trace_planning.h"

typedef struct PlannerTrace
{
	uint64		startTime;

	/* using 0 as invalid val */
	uint64		planningTime;
	HTAB	   *bufferLocksStat;
	char	   *boundParamsLogString;
	bool		isPlanCustom;
} PlannerTrace;


typedef struct GetCachedPlanTrace
{
	CachedPlanSource *plansource;
	ParamListInfo boundParams;
	int64		prevNumCustomPlans;
	int64		prevNumGenericPlans;
} GetCachedPlanTrace;


static PlannerTrace plannerTrace =
{
	.startTime = 0,
	.planningTime = 0,
	.boundParamsLogString = NULL,
	.bufferLocksStat = NULL,
	.isPlanCustom = false
};

static GetCachedPlanTrace getCachedPlanTrace =
{
	.plansource = NULL,
	.prevNumCustomPlans = 0,
	.prevNumGenericPlans = 0
};

static MemoryContext traceMemoryContext = NULL;

static void PlannerTraceInFunc(void *data);
static void PlannerTraceRetFunc(void *data);
static void PlannerTraceCleanFunc(UprobeAttachInterface *uprobe);
static void GetCachedPlanInFunc(void *data, CachedPlanSource *plansource, ParamListInfo boundParams);
static void GetCachedPlanRetFunc(void *data);
static void GetCachedPlanCleanFunc(UprobeAttachInterface *uprobe);

static void
PlannerTraceInFunc(void *data)
{
	struct timespec time;

	LockOnBuffersTraceStatPush();

	clock_gettime(CLOCK_MONOTONIC, &time);

	plannerTrace.startTime = time.tv_sec * 1000000000L + time.tv_nsec;
}


static void
PlannerTraceRetFunc(void *data)
{
	struct timespec time;

	clock_gettime(CLOCK_MONOTONIC, &time);

	plannerTrace.planningTime = time.tv_sec * 1000000000L + time.tv_nsec - plannerTrace.startTime;

	if (plannerTrace.bufferLocksStat)
		hash_destroy(plannerTrace.bufferLocksStat);

	plannerTrace.bufferLocksStat = LockOnBuffersTraceStatPopAndGet();
}


static void
PlannerTraceCleanFunc(UprobeAttachInterface *uprobe)
{
	pfree(uprobe);
}


static void
GetCachedPlanInFunc(void *data, CachedPlanSource *plansource, ParamListInfo boundParams)
{
	getCachedPlanTrace.plansource = plansource;
	getCachedPlanTrace.prevNumCustomPlans = plansource->num_custom_plans;
	getCachedPlanTrace.prevNumGenericPlans = plansource->num_generic_plans;
	getCachedPlanTrace.boundParams = boundParams;
}


static void
GetCachedPlanRetFunc(void *data)
{
	MemoryContext old;

	old = MemoryContextSwitchTo(traceMemoryContext);

	if (plannerTrace.boundParamsLogString)
		pfree(plannerTrace.boundParamsLogString);

	if (getCachedPlanTrace.boundParams)
		plannerTrace.boundParamsLogString = BuildParamLogString(getCachedPlanTrace.boundParams, NULL, -1);
	else
		plannerTrace.boundParamsLogString = pstrdup("");

	if (getCachedPlanTrace.plansource->num_custom_plans != getCachedPlanTrace.prevNumCustomPlans)
	{
		plannerTrace.isPlanCustom = true;
	}
	else if (getCachedPlanTrace.plansource->num_generic_plans != getCachedPlanTrace.prevNumGenericPlans)
	{
		plannerTrace.isPlanCustom = false;
	}
	MemoryContextSwitchTo(old);
}


static void
GetCachedPlanCleanFunc(UprobeAttachInterface *uprobe)
{
	pfree(uprobe);
}

/* return 2 Uprobes to attach in resUrpobes array */
void
PlanningUprobesGet(UprobeAttachInterface **resUprobes, MemoryContext context)
{
	traceMemoryContext = context;

	resUprobes[0] = (UprobeAttachInterface *) palloc0(sizeof(UprobeAttachInterface));
	resUprobes[0]->cleanFunc = PlannerTraceCleanFunc;
	resUprobes[0]->inFunc = PlannerTraceInFunc;
	resUprobes[0]->retFunc = PlannerTraceRetFunc;
	resUprobes[0]->targetSymbol = "planner";

	resUprobes[1] = (UprobeAttachInterface *) palloc0(sizeof(UprobeAttachInterface));
	resUprobes[1]->cleanFunc = GetCachedPlanCleanFunc;
	resUprobes[1]->inFunc = GetCachedPlanInFunc;
	resUprobes[1]->retFunc = GetCachedPlanRetFunc;
	resUprobes[1]->numArgs = 2;
	resUprobes[1]->targetSymbol = "GetCachedPlan";
}


void
PlanningWriteData(void)
{
	if (plannerTrace.planningTime == 0)
		return;

	if (writeMode == TEXT_WRITE_MODE)
		TracePrintf("TRACE PLAN. planningTime %lu nanosec\n", plannerTrace.planningTime);
	else
		TracePrintf("\"planningTime\": \"%lu nanosec\",\n", plannerTrace.planningTime);

	LockOnBuffersTraceWriteStatWithName(plannerTrace.bufferLocksStat, "LWLockPlanning");

	if (plannerTrace.boundParamsLogString == NULL)
	{
		PlanningClearData();
		return;
	}

	if (writeMode == TEXT_WRITE_MODE)
		TracePrintf("TRACE GET_CACHED_PLAN. Custom plan was chosen for boundParams: %s", plannerTrace.boundParamsLogString);
	else
		TracePrintf("    \"planType\": \"%s\",\n    \"params\": \"%s\",\n",
					plannerTrace.isPlanCustom ? "custom" : "generic", plannerTrace.boundParamsLogString);


	PlanningClearData();
}


void
PlanningClearData(void)
{
	hash_destroy(plannerTrace.bufferLocksStat);
	if (plannerTrace.boundParamsLogString)
	{
		pfree(plannerTrace.boundParamsLogString);
		plannerTrace.boundParamsLogString = NULL;
	}
	plannerTrace.bufferLocksStat = NULL;
	plannerTrace.planningTime = 0;
}
