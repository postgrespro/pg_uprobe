#include "postgres.h"
#include <time.h>
#include <unistd.h>
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "libpq/auth.h"

#include "uprobe_message_buffer.h"
#include "uprobe_internal.h"
#include "list.h"
#include "uprobe_shared_config.h"
#include "uprobe_factory.h"
#include "count_uprobes.h"
#include "trace_session.h"

PG_MODULE_MAGIC;
#define DROP_STAT_INTERVAL 10 //in seconds

#define START_SESSION_TRACE_SIG 50
#define STOP_SESSION_TRACE_SIG 51

typedef struct
{
	Uprobe	   *uprobe;
	UprobeAttachType type;
	bool		isShared;
} PGUprobe;


static UprobeList *pguprobeList = NULL;

static uint64 lastTimeDropStat;
#if PG_VERSION_NUM >= 150000
static shmem_request_hook_type prev_shmem_request_hook = NULL;
static void ShmemRequest(void);
#endif
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static ClientAuthentication_hook_type prev_client_auth_hook = NULL;
static bool pendingStartSessionTraceRequest = false;
static bool pendingStopSessionTraceRequest = false;

extern void _PG_init(void);

PG_FUNCTION_INFO_V1(set_uprobe);
PG_FUNCTION_INFO_V1(delete_uprobe);
PG_FUNCTION_INFO_V1(stat_time_uprobe);
PG_FUNCTION_INFO_V1(stat_hist_uprobe);
PG_FUNCTION_INFO_V1(stat_hist_uprobe_simple);
PG_FUNCTION_INFO_V1(list_uprobes);
PG_FUNCTION_INFO_V1(dump_uprobe_stat);
PG_FUNCTION_INFO_V1(start_session_trace);
PG_FUNCTION_INFO_V1(stop_session_trace);
PG_FUNCTION_INFO_V1(start_session_trace_pid);
PG_FUNCTION_INFO_V1(stop_session_trace_pid);
PG_FUNCTION_INFO_V1(start_lockmanager_trace);
PG_FUNCTION_INFO_V1(stop_lockmanager_trace);
PG_FUNCTION_INFO_V1(dump_lockmanager_stat);

static void ShmemStartup(void);
static int	PGUprobeCompare(PGUprobe *uprobe, char *func);
static void StartStatCollector(void);
static void UprobeTimedStatSend(void);
static void PGUprobeLoadFromConfigApplyFunc(const char *func, const char *type);
static void UprobeForStatCollectingInFunc(void *data);
static void UprobeForStatCollectingRetFunc(void *data);
static void SetUprobeForStatCollecting(void);
static void SetUprobeDuringAuntification(Port *port, int value);
static void StartSessionTraceSigHandler(SIGNAL_ARGS);
static void StopSessionTraceSigHandler(SIGNAL_ARGS);
static void WriteHistStatToReturnSet(UprobeAttachHistStat *stat, ReturnSetInfo *rsinfo);

#if PG_VERSION_NUM >= 150000
static void
ShmemRequest(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	MessageBufferRequest();
}
#endif

static void
ShmemStartup(void)
{
	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	MessageBufferInit();
}



static int
PGUprobeCompare(PGUprobe *uprobe, char *func)
{
	return UprobeCompare(uprobe->uprobe, func);
}


static void
StartStatCollector(void)
{
	BackgroundWorker statCollector;

	MemSet(&statCollector, 0, sizeof(BackgroundWorker));
	strcpy(statCollector.bgw_name, "pg_uprobe stat collector");
	strcpy(statCollector.bgw_type, "stat collector");
	statCollector.bgw_flags = BGWORKER_SHMEM_ACCESS;
	statCollector.bgw_start_time = BgWorkerStart_PostmasterStart;
	statCollector.bgw_restart_time = 5;
	strcpy(statCollector.bgw_library_name, "pg_uprobe");
	strcpy(statCollector.bgw_function_name, "StatCollectorMain");

	RegisterBackgroundWorker(&statCollector);
}


static void
UprobeTimedStatSend(void)
{
	LIST_FOREACH(pguprobeList, it)
	{
		PGUprobe   *pgUprobe = (PGUprobe *) it->value;

		if (pgUprobe->isShared)
			UprobeCallTimedCallback(pgUprobe->uprobe);
	}
}

static void
PGUprobeLoadFromConfigApplyFunc(const char *func, const char *type)
{
	Uprobe	   *uprobe;
	UprobeAttach uprobeAttach;
	UPROBE_INIT_RES uprobeAttachRes;

	elog(LOG, "setting uprobe for %s with type %s", func, type);
	CreateUprobeAttachForType(type, func, &uprobeAttach);
	Assert(uprobeAttach.type != INVALID_TYPE);
	uprobeAttachRes = UprobeInit(uprobeAttach.impl, &uprobe);

	if (uprobeAttachRes == SUCCESS)
	{
		PGUprobe   *pg_uprobe = NULL;
		MemoryContext old;

		PG_TRY();
		{
			old = MemoryContextSwitchTo(UprobeMemoryContext);
			pg_uprobe = palloc(sizeof(PGUprobe));
			MemoryContextSwitchTo(old);
			pg_uprobe->uprobe = uprobe;
			pg_uprobe->isShared = true;
			pg_uprobe->type = uprobeAttach.type;
			ListAdd(pguprobeList, pg_uprobe);
		}
		PG_CATCH();
		{
			UprobeDelete(uprobe);
			pfree(pg_uprobe);
			/* we don't clean listAdd, because it's the last thing to fail */
			elog(LOG, "can not store info about set uprobe");
			PG_RE_THROW();
		}
		PG_END_TRY();
	}
	else
		elog(ERROR, "can not make uprobe on function %s", func);
}


static void
UprobeForStatCollectingInFunc(void *data)
{
}

static void
UprobeForStatCollectingRetFunc(void *data)
{
	struct timespec time;

	clock_gettime(CLOCK_MONOTONIC, &time);
	if (time.tv_sec - lastTimeDropStat > DROP_STAT_INTERVAL)
	{
		UprobeTimedStatSend();
		lastTimeDropStat = time.tv_sec;
	}
	if (pendingStartSessionTraceRequest)
	{
		pendingStartSessionTraceRequest = false;

		PG_TRY();
		{
			SessionTraceStart();
		}
		PG_CATCH();
		{
			elog(LOG, "can't start session trace at pid %d", getpid());
		}
		PG_END_TRY();

	}

	if (pendingStopSessionTraceRequest)
	{
		pendingStopSessionTraceRequest = false;
		PG_TRY();
		{
			SessionTraceStop(true);
		}
		PG_CATCH();
		{
			elog(LOG, "can't stop session trace at pid %d", getpid());
		}
		PG_END_TRY();
	}
}


static void
SetUprobeForStatCollecting(void)
{
	Uprobe	   *uprobeUnused;	/* we don't really care about this uprobe
								 * after seting */
	UPROBE_INIT_RES res;
	struct timespec time;
	UprobeAttachInterface *uprobe = palloc(sizeof(UprobeAttachInterface));

	uprobe->cleanFunc = (UprobeAttachCleanFunc) pfree;
	uprobe->data = NULL;
	uprobe->inFunc = UprobeForStatCollectingInFunc;
	uprobe->needRetVal = false;
	uprobe->numArgs = 0;
	uprobe->retFunc = UprobeForStatCollectingRetFunc;
	uprobe->targetSymbol = "ReadyForQuery";
	uprobe->timedCallback = NULL;
	res = UprobeInit(uprobe, &uprobeUnused);
	if (res != SUCCESS)
	{
		elog(ERROR, "can't set uprobe for collecting stat over time on symbol ReadyForQuery");
	}
	clock_gettime(CLOCK_MONOTONIC, &time);
	lastTimeDropStat = time.tv_sec;
}


static void
SetUprobeDuringAuntification(Port *port, int value)
{
	MemoryContext old;

	if (prev_client_auth_hook)
		prev_client_auth_hook(port, value);

	old = MemoryContextSwitchTo(UprobeMemoryContext);
	ListInit(&pguprobeList, (CompareFunction) PGUprobeCompare, UprobeMemoryContext);
	SetUprobeForStatCollecting();
	before_shmem_exit((pg_on_exit_callback) UprobeTimedStatSend, (Datum) 0);

	PG_TRY();
	{
		PGUprobeLoadFromSharedConfig(&PGUprobeLoadFromConfigApplyFunc);
	}
	PG_CATCH();
	{
		LIST_FOREACH(pguprobeList, it)
		{
			UprobeDelete(((PGUprobe *) it->value)->uprobe);
			pfree((PGUprobe *) it->value);
		}
		ListMakeEmpty(pguprobeList);
		elog(WARNING, "error during setting uprobes");
	}
	PG_END_TRY();
	MemoryContextSwitchTo(old);
	signal(START_SESSION_TRACE_SIG, StartSessionTraceSigHandler);
	signal(STOP_SESSION_TRACE_SIG, StopSessionTraceSigHandler);
}


static void
WriteHistStatToReturnSet(UprobeAttachHistStat *stat, ReturnSetInfo *rsinfo)
{
	double		start = stat->start;
	double		stop = stat->stop;
	double		step = stat->step;

	for (size_t i = 0; i < stat->histArraySize; i++)
	{
		Datum		histValues[3];
		bool		histNulls[3] = {false, false, false};
		char		rangeBuffer[128];
		char		histEntryBuffer[51];
		double		percent = ((double) stat->histArray[i] / (double) stat->totalCalls) * 100.0;
		size_t		percentForHistEntry = (size_t) (percent / 2.0);
		int64_t		percentForNumeric = (size_t) (percent * 1000.0);

		if (i == 0)
			sprintf(rangeBuffer, "(..., %.1lf us)", start);
		else if (i == stat->histArraySize - 1)
			sprintf(rangeBuffer, "(%.1lf us, ...)", stop);
		else
			sprintf(rangeBuffer, "(%.1lf us, %.1lf us)", start + (i - 1) * step, start + i * step);

		memset(histEntryBuffer, '@', percentForHistEntry);
		memset(histEntryBuffer + percentForHistEntry, ' ', 50 - percentForHistEntry);
		histEntryBuffer[50] = '\0';

		histValues[0] = CStringGetTextDatum(rangeBuffer);
		histValues[1] = CStringGetTextDatum(histEntryBuffer);
		histValues[2] = NumericGetDatum(int64_div_fast_to_numeric(percentForNumeric, 3));

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, histValues, histNulls);
	}
}


void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
	{
		elog(ERROR, "start pg_uprobe extension at shared_preload_libraries");
		return;
	}
	if (MakePGDirectory("./pg_uprobe") < 0 && errno != EEXIST)
		elog(ERROR, "can't create subdir pg_uprobe in PG_DATA");

#if PG_VERSION_NUM >= 150000
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = ShmemRequest;
#else
	MessageBufferRequest();
#endif

	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = ShmemStartup;

	prev_client_auth_hook = ClientAuthentication_hook;
	ClientAuthentication_hook = SetUprobeDuringAuntification;
	UprobeInternalInit();
	StartStatCollector();
	SessionTraceInit();
}


Datum
set_uprobe(PG_FUNCTION_ARGS)
{
	text	   *funcName;
	text	   *uprobeType;
	MemoryContext old;
	bool		isShared;
	char	   *func;
	char	   *type;
	UPROBE_INIT_RES setUprobeRes;
	Uprobe	   *uprobe;
	UprobeAttach uprobeAttach;

	funcName = PG_GETARG_TEXT_PP(0);
	uprobeType = PG_GETARG_TEXT_PP(1);
	isShared = PG_GETARG_BOOL(2);
	func = text_to_cstring(funcName);
	type = text_to_cstring(uprobeType);

	if (ListContains(pguprobeList, func))
	{
		elog(ERROR, "Uprobe on this function already exists");
	}
	old = MemoryContextSwitchTo(UprobeMemoryContext);
	CreateUprobeAttachForType(type, func, &uprobeAttach);
	if (uprobeAttach.type == INVALID_TYPE)
		elog(ERROR, "Invalid uprobe type given. Supported types: TIME, HIST, MEM");

	setUprobeRes = UprobeInit(uprobeAttach.impl, &uprobe);

	if (setUprobeRes == SUCCESS)
	{
		PGUprobe   *pguprobe = NULL;
		volatile bool isInsertedInSharedConfig = false;
		volatile bool isPguprobeAlocated = false;

		PG_TRY();
		{
			if (isShared)
			{
				MessageNewSharedUprobe mes;

				mes.base.type = MESSAGE_NEW_SHARED_UPROBE;
				mes.uprobeType = uprobeAttach.type;
				mes.base.size = sizeof(MessageNewSharedUprobe);
				MessageBufferPut((Message *) &mes, 1, func);
				PGUprobeSaveInSharedConfig(func, type);
				isInsertedInSharedConfig = true;
			}
			pguprobe = palloc(sizeof(PGUprobe));
			isPguprobeAlocated = true;
			pguprobe->uprobe = uprobe;
			pguprobe->isShared = isShared;
			pguprobe->type = uprobeAttach.type;
			ListAdd(pguprobeList, pguprobe);
		}
		PG_CATCH();
		{
			UprobeDelete(uprobe);
			if (isPguprobeAlocated)
				pfree(pguprobe);
			if (isInsertedInSharedConfig)
			{
				MessageDeleteSharedUprobe mes;

				mes.base.type = MESSAGE_DELETE_SHARED_UPROBE;
				mes.shouldWriteStat = false;
				mes.base.size = sizeof(MessageDeleteSharedUprobe);
				MessageBufferPut((Message *) &mes, 1, func);
				PGUprobeDeleteFromSharedConfig(func);
			}
			/* we don't clean listAdd, because it's the last thing to fail */
			PG_RE_THROW();
		}
		PG_END_TRY();
	}
	pfree(func);
	pfree(type);
	MemoryContextSwitchTo(old);
	switch (setUprobeRes)
	{
		case INVALID_NUMBER_OF_ARGS:
		case INTERNAL_ERROR:
			elog(ERROR, "Internal error during setting uprobe");
			break;
		case CANNOT_FIND_SYMBOL:
			elog(ERROR, "Can't find given function.");
			break;
		default:
			PG_RETURN_TEXT_P(funcName);
	}

}


Datum
delete_uprobe(PG_FUNCTION_ARGS)
{
	text	   *funcName;
	char	   *func;
	PGUprobe   *popValue;
	bool		shouldWriteStat;

	funcName = PG_GETARG_TEXT_PP(0);
	shouldWriteStat = PG_GETARG_BOOL(1);
	func = text_to_cstring(funcName);
	popValue = ListPop(pguprobeList, func);
	if (popValue)
	{
		UprobeDelete(popValue->uprobe);
		if (popValue->isShared)
		{
			MessageDeleteSharedUprobe mes;

			/* sending data that our process has */
			UprobeTimedStatSend();
			if (shouldWriteStat)
				ResetLatch(MyLatch);

			mes.base.type = MESSAGE_DELETE_SHARED_UPROBE;
			mes.shouldWriteStat = shouldWriteStat;
			mes.base.size = sizeof(MessageDeleteSharedUprobe);
			mes.latch = MyLatch;
			MessageBufferPut((Message *) &mes, 1, func);
			PGUprobeDeleteFromSharedConfig(func);

			/* waiting till data is written */
			if (shouldWriteStat)
				WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, 0, END_OF_WRITE_STAT_EVENT);
		}
		pfree(popValue);
	}
	pfree(func);
	PG_RETURN_VOID();
}


Datum
stat_time_uprobe(PG_FUNCTION_ARGS)
{
	char	   *buffer;
	UprobeAttachTimeStat *stat;
	text	   *result;
	text	   *funcName;
	char	   *func;
	PGUprobe   *uprobe = NULL;

	funcName = PG_GETARG_TEXT_PP(0);
	func = text_to_cstring(funcName);

	LIST_FOREACH(pguprobeList, it)
	{
		if (!PGUprobeCompare((PGUprobe *) it->value, func))
		{
			uprobe = (PGUprobe *) it->value;
			break;
		}
	}
	if (uprobe == NULL)
		elog(ERROR, "no uprobe named %s", func);

	if (uprobe->type != TIME)
		elog(ERROR, "Can't get TIME statistic on non TIME uprobe");

	stat = UprobeAttachTimeGetStat(UprobeGetAttachInterface(uprobe->uprobe));
	buffer = psprintf("calls: %ld  avg time: %ld ns", stat->numCalls, stat->avgTime);
	result = cstring_to_text(buffer);
	pfree(buffer);
	PG_RETURN_TEXT_P(result);
}

#if PG_VERSION_NUM < 150000
static void
InitMaterializedSRF(FunctionCallInfo fcinfo, int flags)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("pg_urpobe: set-valued function called in context that cannot accept a set")));

	/* Switch into long-lived context to construct returned data structures */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &rsinfo->setDesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "pg_uprobe: return type must be a row type");

	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tuplestore_begin_heap(true, false, work_mem);

	MemoryContextSwitchTo(oldcontext);
}
#endif

Datum
stat_hist_uprobe(PG_FUNCTION_ARGS)
{
	UprobeAttachHistStat *stat;
	double		start;
	double		stop;
	double		step;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	char	   *func;
	text	   *funcName;
	PGUprobe   *uprobe = NULL;

	funcName = PG_GETARG_TEXT_PP(0);
	func = text_to_cstring(funcName);

	LIST_FOREACH(pguprobeList, it)
	{
		if (!PGUprobeCompare((PGUprobe *) it->value, func))
		{
			uprobe = (PGUprobe *) it->value;
			break;
		}
	}

	if (uprobe == NULL)
		elog(ERROR, "can't find uprobe named %s", func);

	if (uprobe->type != HIST)
		elog(ERROR, "Can't get HIST statistic on non HIST uprobe");

	start = PG_GETARG_FLOAT8(1);
	stop = PG_GETARG_FLOAT8(2);
	step = PG_GETARG_FLOAT8(3);

	if (start > stop)
		elog(ERROR, "start argument can't be grater than stop argument");

	if (step <= 0.0)
		elog(ERROR, "step argument can't be negative or zero");

	stat = UprobeAttachHistGetStat(UprobeGetAttachInterface(uprobe->uprobe), start, stop, step);
	InitMaterializedSRF(fcinfo, 0);
	if (stat->totalCalls == 0)
	{
		elog(INFO, "symbol %s wasn't called", func);
		PG_RETURN_NULL();
	}

	WriteHistStatToReturnSet(stat, rsinfo);

	PG_RETURN_VOID();
}


Datum
stat_hist_uprobe_simple(PG_FUNCTION_ARGS)
{
	UprobeAttachHistStat *stat;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	char	   *func;
	text	   *funcName;
	PGUprobe   *uprobe = NULL;

	funcName = PG_GETARG_TEXT_PP(0);
	func = text_to_cstring(funcName);

	LIST_FOREACH(pguprobeList, it)
	{
		if (!PGUprobeCompare((PGUprobe *) it->value, func))
		{
			uprobe = (PGUprobe *) it->value;
			break;
		}
	}

	if (uprobe == NULL)
		elog(ERROR, "can't find uprobe named %s", func);

	if (uprobe->type != HIST)
		elog(ERROR, "Can't get HIST statistic on non HIST uprobe");

	stat = UprobeAttachHistGetStatSimple(UprobeGetAttachInterface(uprobe->uprobe));
	InitMaterializedSRF(fcinfo, 0);
	if (stat->totalCalls == 0)
	{
		elog(INFO, "symbol %s wasn't called", func);
		PG_RETURN_NULL();
	}

	WriteHistStatToReturnSet(stat, rsinfo);

	PG_RETURN_VOID();
}


Datum
list_uprobes(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Datum		listValues[3];
	bool		listNulls[3] = {false, false, false};

	InitMaterializedSRF(fcinfo, 0);

	LIST_FOREACH(pguprobeList, it)
	{
		PGUprobe   *pg_uprobe = (PGUprobe *) it->value;

		listValues[0] = CStringGetTextDatum(UprobeGetFunc(pg_uprobe->uprobe));
		listValues[1] = CStringGetTextDatum(GetCharNameForUprobeAttachType(pg_uprobe->type));
		listValues[2] = BoolGetDatum(pg_uprobe->isShared);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, listValues, listNulls);
	}
	PG_RETURN_VOID();
}


Datum
dump_uprobe_stat(PG_FUNCTION_ARGS)
{
	bool		should_empty_stat = PG_GETARG_BOOL(1);
	text	   *func_name = PG_GETARG_TEXT_P(0);
	char	   *func;
	MessageWriteStat mes;

	/* sending data that our process has */
	UprobeTimedStatSend();

	ResetLatch(MyLatch);
	mes.base.type = MESSAGE_WRITE_STAT;
	mes.shouldEmptyData = should_empty_stat;
	mes.base.size = sizeof(MessageWriteStat);
	mes.latch = MyLatch;
	func = text_to_cstring(func_name);
	MessageBufferPut((Message *) &mes, 1, func);
	/* waiting till data is written */
	WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, 0, END_OF_WRITE_STAT_EVENT);
	PG_RETURN_VOID();
}


Datum
start_session_trace(PG_FUNCTION_ARGS)
{
	SessionTraceStart();
	PG_RETURN_VOID();
}


Datum
stop_session_trace(PG_FUNCTION_ARGS)
{
	SessionTraceStop(false);
	PG_RETURN_VOID();
}


Datum
start_session_trace_pid(PG_FUNCTION_ARGS)
{
	if (kill(PG_GETARG_INT32(0), START_SESSION_TRACE_SIG))
	{
		ereport(ERROR,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("failed to send signal %d to backend pid %d: %m",
						START_SESSION_TRACE_SIG, PG_GETARG_INT32(0))));
	}
	PG_RETURN_VOID();
}


Datum
stop_session_trace_pid(PG_FUNCTION_ARGS)
{
	if (kill(PG_GETARG_INT32(0), STOP_SESSION_TRACE_SIG))
	{
		ereport(ERROR,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("failed to send signal %d to backend pid %d: %m",
						STOP_SESSION_TRACE_SIG, PG_GETARG_INT32(0))));
	}
	PG_RETURN_VOID();
}


static void
StartSessionTraceSigHandler(SIGNAL_ARGS)
{
	pendingStartSessionTraceRequest = true;
}


static void
StopSessionTraceSigHandler(SIGNAL_ARGS)
{
	pendingStopSessionTraceRequest = true;
}


Datum
start_lockmanager_trace(PG_FUNCTION_ARGS)
{
	DirectFunctionCall3(set_uprobe, (Datum) cstring_to_text("LWLockAcquire"), (Datum) cstring_to_text("LOCK_AQUIRE"), BoolGetDatum(true));
	PG_TRY();
	{
		DirectFunctionCall3(set_uprobe, (Datum) cstring_to_text("LWLockRelease"), (Datum) cstring_to_text("LOCK_RELEASE"), BoolGetDatum(true));
	}
	PG_CATCH();
	{
		DirectFunctionCall2(delete_uprobe, (Datum) cstring_to_text("LWLockAcquire"), BoolGetDatum(false));
		PG_RE_THROW();
	}
	PG_END_TRY();


	PG_RETURN_VOID();
}


Datum
stop_lockmanager_trace(PG_FUNCTION_ARGS)
{
	DirectFunctionCall2(delete_uprobe, (Datum) cstring_to_text("LWLockAcquire"), BoolGetDatum(PG_GETARG_BOOL(0)));
	DirectFunctionCall2(delete_uprobe, (Datum) cstring_to_text("LWLockRelease"), BoolGetDatum(PG_GETARG_BOOL(0)));
	PG_RETURN_VOID();
}


Datum
dump_lockmanager_stat(PG_FUNCTION_ARGS)
{
	DirectFunctionCall2(dump_uprobe_stat, (Datum) cstring_to_text("LWLockRelease"), BoolGetDatum(PG_GETARG_BOOL(0)));
	PG_RETURN_VOID();
}
