#include "postgres.h"
#include <time.h>
#include <sys/time.h>
#include "utils/memutils.h"
#include "utils/portal.h"
#include "utils/jsonb.h"
#include "utils/guc.h"
#if PG_MAJORVERSION_NUM >= 18
#include "commands/explain_state.h"
#include "commands/explain_format.h"
#endif
#include "commands/explain.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "tcop/utility.h"
#include "trace_parsing.h"
#include "uprobe_internal.h"
#include "trace_lock_on_buffers.h"
#include "trace_execute_nodes.h"
#include "trace_planning.h"
#include "list.h"
#include "trace_wait_events.h"
#include "json_to_jsonbvalue_parser.h"
#include "trace_file.h"

#include "trace_session.h"

#define CLOCKTYPE CLOCK_MONOTONIC
#define MAX_PLAN_NEST_LEVEL 100


static UprobeList *uprobesList = NULL;

static MemoryContext traceMemoryContext = NULL;

/* We need it in case that ListAdd fails and we need to delete this uprobe */
static volatile Uprobe *lastSetUprobe = NULL;

static ExecutorRun_hook_type prev_ExecutorRun_hook = NULL;
static ExecutorStart_hook_type prev_ExecutorStart_hook = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish_hook = NULL;
static ProcessUtility_hook_type prev_ProcessUtility_hook = NULL;

bool		isExecuteTime = false;

int			ExecutorRunNestLevel = 0;

/* GUC parametrs */
int			writeMode = JSON_WRITE_MODE;
bool		traceLWLocksForEachNode = true;
bool		writeOnlySleepLWLocksStat = true;


static const struct config_enum_entry writeModeOptions[] = {
	{"text", TEXT_WRITE_MODE, false},
	{"json", JSON_WRITE_MODE, false},
	{NULL, 0, false}
};

#if PG_MAJORVERSION_NUM >= 18
static void TraceSessionExecutorRun(QueryDesc *queryDesc,
									ScanDirection direction,
									uint64 count);
#else
static void TraceSessionExecutorRun(QueryDesc *queryDesc,
									ScanDirection direction,
									uint64 count,
									bool execute_once);
#endif
static void TraceSessionExecutorProcessUtility(PlannedStmt *pstmt,
											   const char *queryString,
											   bool readOnlyTree,
											   ProcessUtilityContext context,
											   ParamListInfo params,
											   QueryEnvironment *queryEnv,
											   DestReceiver *dest,
											   QueryCompletion *qc);
static void TraceSessionExecutorStart(QueryDesc *queryDesc, int eflags);
static void TraceSessionExecutorFinish(QueryDesc *queryDesc);
static char *ProcessDescBeforeExec(QueryDesc *queryDesc);

static UprobeList *MakeNodesPlanStringsText(char *startPlanExplain, char *endPlanExplain);

static JsonbValue *FindField(JsonbValue *json, char *field, size_t len);
static void JsonbDeleteField(JsonbValue *json, JsonbValue *value);
static void AddExplainJsonbToNode(JsonbValue *planNode, UprobeList *strNodes);
static void MakeNodesRecursive(JsonbValue *planNode, UprobeList *strNodes);
static UprobeList *MakeNodesPlanStringsJson(char *explainString, size_t len);

static void SetUprobes2Ptr(UprobeAttachInterface **uprobes, size_t size);
static void SetUprobes1Ptr(UprobeAttachInterface *uprobes, size_t size);

static char *BeforeExecution(QueryDesc *queryDesc);
static void AfterExecution(QueryDesc *queryDesc, char *planString);


static void
SetUprobes2Ptr(UprobeAttachInterface **uprobes, size_t size)
{
	Uprobe	   *uprobe;
	UPROBE_INIT_RES res;

	for (size_t i = 0; i < size; i++)
	{
		res = UprobeInit(uprobes[i], &uprobe);
		if (res != SUCCESS)
		{
			elog(ERROR, "can't set uprobe for %s symbol", uprobes[i]->targetSymbol);
		}
		lastSetUprobe = uprobe;
		/* save it in case ListAdd faild */
		ListAdd(uprobesList, uprobe);
		lastSetUprobe = NULL;
	}
}


static void
SetUprobes1Ptr(UprobeAttachInterface *uprobes, size_t size)
{
	Uprobe	   *uprobe;
	UPROBE_INIT_RES res;

	for (size_t i = 0; i < size; i++)
	{
		res = UprobeInit(&uprobes[i], &uprobe);
		if (res != SUCCESS)
		{
			elog(ERROR, "can't set uprobe for %s symbol", uprobes[i].targetSymbol);
		}
		lastSetUprobe = uprobe;
		/* save it in case ListAdd faild */
		ListAdd(uprobesList, uprobe);
		lastSetUprobe = NULL;
	}
}


static char *
BeforeExecution(QueryDesc *queryDesc)
{
	struct timeval timeOfDay;
	char	   *planCopy;
	char		timebuf[128];
	time_t		tt;

	LockOnBuffersTraceStatPush();
	TraceWaitEventsClearStat();
	ExecuteNodesStatePush(queryDesc);

	if (writeMode == JSON_WRITE_MODE)
		TracePrintf("{");

	ParsingWriteData();
	PlanningWriteData();

	gettimeofday(&timeOfDay, NULL);
	tt = (time_t) timeOfDay.tv_sec;
	strftime(timebuf, sizeof(timebuf), "%Y:%m:%dT%H:%M:%S", localtime(&tt));
	snprintf(timebuf + strlen(timebuf), sizeof(timebuf) - strlen(timebuf),
			 ".%03d", (int) (timeOfDay.tv_usec / 1000));
	if (writeMode == JSON_WRITE_MODE)
		TracePrintf("\"executionStart\": \"%s\",\n", timebuf);
	else
		TracePrintf("Execution start: %s\n", timebuf);
	planCopy = ProcessDescBeforeExec(queryDesc);
	isExecuteTime = true;
	isFirstNodeCall = true;
	if (writeMode == JSON_WRITE_MODE)
		TracePrintf("\"executionEvents\": [\n");

	return planCopy;
}


static void
AfterExecution(QueryDesc *queryDesc, char *planString)
{
	StringInfoData str;
	MemoryContext old;
	bool		hasStat;

	isExecuteTime = false;
	/* finishing race session file in case trace session has ended */
	if (!traceMemoryContext)
	{
		if (writeMode == JSON_WRITE_MODE)
		{
			TracePrintf("\n}");
			if (ExecutorRunNestLevel == 0)
				TracePrintf("]}");
		}
		if (ExecutorRunNestLevel == 0)
			CloseTraceSessionFile();
		return;
	}

	ExecutorTraceDumpAndClearStat(queryDesc, planString);
	ExecuteNodesStatePop();
	old = MemoryContextSwitchTo(traceMemoryContext);
	initStringInfo(&str);

	hasStat = LockOnBuffersTraceWriteStat(&str, false);
	if (hasStat)
	{
		if (writeMode == TEXT_WRITE_MODE)
			TracePrintf("TRACE LWLOCK. Buffer locks stat inside PortalRun %s\n", str.data);
		else
			TracePrintf(",\n\"locksInsidePortalRun\": %s", str.data);
	}
	LockOnBuffersTraceStatPop();

	resetStringInfo(&str);
	hasStat = LockOnBuffersTraceWriteStat(&str, true);
	if (hasStat)
	{
		if (writeMode == TEXT_WRITE_MODE)
			TracePrintf("TRACE LWLOCK. Buffer locks stat inside PortalRun %s\n", str.data);
		else
			TracePrintf(",\n\"locksOutsidePortalRun\": %s", str.data);
	}
	resetStringInfo(&str);
	hasStat = TraceWaitEventDumpStat(&str);
	if (hasStat)
	{
		if (writeMode == TEXT_WRITE_MODE)
			TracePrintf("TRACE WAIT EVENTS.\n %s\n", str.data);
		else
			TracePrintf(",\n\"waitEventStat\": %s\n", str.data);
	}
	pfree(str.data);
	MemoryContextSwitchTo(old);

	if (writeMode == TEXT_WRITE_MODE)
		TracePrintf("\n\n---------query end------------\n\n");
	else
		TracePrintf("}");
}


static void
TraceNotNormalShutdown(void)
{
	if (!traceFile)
		return;
	if (writeMode == JSON_WRITE_MODE)
	{

		if (ExecutorRunNestLevel > 0)

			/*
			 * We clone array of ExecutorEvents and whole json oject for each
			 * nestlevel.
			 */
			for (int i = 0; i < ExecutorRunNestLevel; i++)
				TracePrintf("]\n}");

		TracePrintf("\n]}");
	}
	CloseTraceSessionFile();
}


void
SessionTraceStart(void)
{
	UprobeAttachInterface *fixedUprobes[2];
	UprobeAttachInterface *uprobes;
	MemoryContext old;
	size_t		size;

	if (uprobesList)
		elog(ERROR, "session trace is already ON");

	PG_TRY();
	{
		traceMemoryContext = AllocSetContextCreate(TopMemoryContext, "uprobe trace context", ALLOCSET_DEFAULT_SIZES);
		OpenTraceSessionFile(true);

		ListInit(&uprobesList, (CompareFunction) UprobeCompare, traceMemoryContext);
		old = MemoryContextSwitchTo(traceMemoryContext);

		LockOnBuffersUprobesGet(traceMemoryContext, fixedUprobes, writeOnlySleepLWLocksStat);
		SetUprobes2Ptr(fixedUprobes, 2);

		uprobes = ParsingUprobeGet();
		SetUprobes1Ptr(uprobes, 1);

		PlanningUprobesGet(fixedUprobes, traceMemoryContext);
		SetUprobes2Ptr(fixedUprobes, 2);

		ExecutorTraceUprobesGet(fixedUprobes, traceMemoryContext, traceLWLocksForEachNode);
		SetUprobes2Ptr(fixedUprobes, 1);

		uprobes = TraceWaitEventsUprobesGet(&size);
		SetUprobes1Ptr(uprobes, size);

		on_proc_exit((pg_on_exit_callback) TraceNotNormalShutdown, 0);
		prev_ExecutorRun_hook = ExecutorRun_hook;
		ExecutorRun_hook = TraceSessionExecutorRun;
		prev_ExecutorStart_hook = ExecutorStart_hook;
		ExecutorStart_hook = TraceSessionExecutorStart;
		prev_ExecutorFinish_hook = ExecutorFinish_hook;
		ExecutorFinish_hook = TraceSessionExecutorFinish;
		prev_ProcessUtility_hook = ProcessUtility_hook;
		ProcessUtility_hook = TraceSessionExecutorProcessUtility;
	}
	PG_CATCH();
	{
		CloseTraceSessionFile();
		if (!uprobesList)
			PG_RE_THROW();

		LIST_FOREACH(uprobesList, it)
		{
			UprobeDelete(it->value);
		}
		ListFree(uprobesList);
		uprobesList = NULL;
		traceMemoryContext = NULL;
		if (lastSetUprobe)
			UprobeDelete((Uprobe *) lastSetUprobe);
		lastSetUprobe = NULL;
		TraceWaitEventsUprobesClean();
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (writeMode == JSON_WRITE_MODE)
	{
		TracePrintf("{\"pid\":%d,\n \"queries\": [\n", MyProc->pid);
	}
	else
	{
		TracePrintf("process pid %d\n", MyProc->pid);
	}

	MemoryContextSwitchTo(old);
}


void
SessionTraceStop(bool closeFile)
{
	if (uprobesList == NULL)
		elog(ERROR, "session trace is not ON");
	ExecutorTraceStop();
	LIST_FOREACH(uprobesList, it)
	{
		UprobeDelete(it->value);
	}
	uprobesList = NULL;
	if (closeFile)
	{
		TracePrintf("\n]}");
		CloseTraceSessionFile();
	}
	/* Else TraceSessionFile will be closed at TraceSessionExecutorRun. */
	PlanningClearData();
	ParsingClearData();
	MemoryContextDelete(traceMemoryContext);
	traceMemoryContext = NULL;
	ExecutorRun_hook = prev_ExecutorRun_hook;
	ExecutorStart_hook = prev_ExecutorStart_hook;
}


void
SessionTraceInit(void)
{
	DefineCustomEnumVariable("pg_uprobe.trace_write_mode",
							 "format for session trace output",
							 NULL,
							 &writeMode,
							 JSON_WRITE_MODE,
							 writeModeOptions,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);
	DefineCustomBoolVariable("pg_uprobe.trace_LWLocks_for_each_node",
							 "if set to true LWLock stat will be traced for each node execution otherwise it will be traced for whole execution",
							 NULL,
							 &traceLWLocksForEachNode,
							 true,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);
	DefineCustomBoolVariable("pg_uprobe.write_only_sleep_LWLocks_stat",
							 "if set to true LWLock stat will be traced only for those locks that you had to fall asleep to take.",
							 NULL,
							 &writeOnlySleepLWLocksStat,
							 true,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);
	TraceFileDeclareGucVariables();
}


static char *
ProcessDescBeforeExec(QueryDesc *queryDesc)
{
	MemoryContext old;
	ExplainState *es;
	size_t		explainPlanStartOffset;
	size_t		explainPlanEnd;
	UprobeList *stringPlanParts = NULL;
	char	   *planCopy;

	old = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);
	es = NewExplainState();

	es->analyze = queryDesc->instrument_options;
	es->verbose = true;
	es->buffers = es->analyze;
	es->wal = es->analyze;
	es->timing = es->analyze;
	es->summary = es->analyze;
	if (writeMode == TEXT_WRITE_MODE)
		es->format = EXPLAIN_FORMAT_TEXT;
	else
		es->format = EXPLAIN_FORMAT_JSON;
	es->settings = true;

	ExplainBeginOutput(es);
	ExplainQueryText(es, queryDesc);
#if PG_MAJORVERSION_NUM > 15
	ExplainQueryParameters(es, queryDesc->params, -1);
#endif
	explainPlanStartOffset = es->str->len;
	ExplainPrintPlan(es, queryDesc);
	explainPlanEnd = es->str->len;
	if (es->analyze)
		ExplainPrintTriggers(es, queryDesc);
	if (es->costs)
		ExplainPrintJITSummary(es, queryDesc);
	ExplainEndOutput(es);

	/* Remove last line break */
	if (es->str->len > 0 && es->str->data[es->str->len - 1] == '\n')
		es->str->data[--es->str->len] = '\0';
	/* Fix JSON to output an object */
	if (es->format == EXPLAIN_FORMAT_JSON)
	{
		es->str->data[0] = '{';
		es->str->data[es->str->len - 1] = '}';
	}

	if (writeMode == TEXT_WRITE_MODE)
		TracePrintf("TRACE EXECUTE. plan: \n%s\n", es->str->data);
	else
		TracePrintf("\"explain\": %s,\n", es->str->data);

	if (writeMode == TEXT_WRITE_MODE)
	{
		planCopy = pnstrdup(es->str->data + explainPlanStartOffset, explainPlanEnd - explainPlanStartOffset);
		if (ExecuteNodesStateNeedInit())
			stringPlanParts = MakeNodesPlanStringsText(es->str->data + explainPlanStartOffset, es->str->data + explainPlanEnd);
	}
	else
	{
		planCopy = pnstrdup(es->str->data, es->str->len);
		if (ExecuteNodesStateNeedInit())
			stringPlanParts = MakeNodesPlanStringsJson(es->str->data, es->str->len);
	}
	if (stringPlanParts)
	{
		InitExecutorNodes(queryDesc->planstate, stringPlanParts);
		ListFree(stringPlanParts);
	}

	MemoryContextSwitchTo(old);
	return planCopy;
}

static void
CallOriginalExecutorFinish(PlannedStmt *pstmt,
						   const char *queryString,
						   bool readOnlyTree,
						   ProcessUtilityContext context,
						   ParamListInfo params,
						   QueryEnvironment *queryEnv,
						   DestReceiver *dest,
						   QueryCompletion *qc)
{
	if (prev_ProcessUtility_hook)
			(*prev_ProcessUtility_hook) (pstmt, queryString, readOnlyTree,
										 context, params, queryEnv,
										 dest, qc);
		else
			standard_ProcessUtility(pstmt, queryString, readOnlyTree,
									context, params, queryEnv,
									dest, qc);
}


#if PG_MAJORVERSION_NUM >= 18
static void
TraceSessionExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count)
#else
static void
TraceSessionExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count, bool execute_once)
#endif
{
	char	   *planCopy;
	struct timespec time;
	uint64		executionStarted;

	if (writeMode == JSON_WRITE_MODE && !isFirstNodeCall)
	{
		TracePrintf(",\n");
	}

	planCopy = BeforeExecution(queryDesc);

	clock_gettime(CLOCKTYPE, &time);
	executionStarted = time.tv_sec * 1000000000L + time.tv_nsec;

	ExecutorRunNestLevel++;

	PG_TRY();
	{
#if PG_MAJORVERSION_NUM >= 18
		if (prev_ExecutorRun_hook)
			(*prev_ExecutorRun_hook) (queryDesc, direction, count);
		else
			standard_ExecutorRun(queryDesc, direction, count);
#else
		if (prev_ExecutorRun_hook)
			(*prev_ExecutorRun_hook) (queryDesc, direction, count, execute_once);
		else
			standard_ExecutorRun(queryDesc, direction, count, execute_once);
#endif
	}
	PG_FINALLY();
	{
		uint64		timeDiff;

		ExecutorRunNestLevel--;

		clock_gettime(CLOCKTYPE, &time);
		timeDiff = time.tv_sec * 1000000000L + time.tv_nsec - executionStarted;
		if (writeMode == TEXT_WRITE_MODE)
			TracePrintf("TRACE. Execution finished for %lu nanosec\n", timeDiff);
		else
			TracePrintf("\n],\n\"executionTime\": \"%lu nanosec\"\n", timeDiff);

		AfterExecution(queryDesc, planCopy);
		isFirstNodeCall = false;
	}
	PG_END_TRY();
}


static void
TraceSessionExecutorStart(QueryDesc *queryDesc, int eflags)
{
	ExecuteNodesStateNew(queryDesc);
	PG_TRY();
	{
		if (prev_ExecutorStart_hook)
			(*prev_ExecutorStart_hook) (queryDesc, eflags);
		else
			standard_ExecutorStart(queryDesc, eflags);
	}
	PG_CATCH();
	{
		ExecuteNodesStatePop();
		ExecuteNodesStateDelete(queryDesc);
		PG_RE_THROW();
	}
	PG_END_TRY();

	ExecuteNodesDeleteRegister(queryDesc);
	ExecuteNodesStatePop();
}


static void
TraceSessionExecutorFinish(QueryDesc *queryDesc)
{
	char	   *planCopy;
	struct timespec time;
	uint64		executionStarted;

	/*No Executor nodes will be called, so no need for additional set up*/
	if (queryDesc->estate->es_auxmodifytables == NULL)
	{
		if (prev_ExecutorFinish_hook)
			(*prev_ExecutorFinish_hook) (queryDesc);
		else
			standard_ExecutorFinish(queryDesc);
		return;
	}

	if (writeMode == JSON_WRITE_MODE && !isFirstNodeCall)
	{
		TracePrintf(",\n");
	}

	planCopy = BeforeExecution(queryDesc);

	clock_gettime(CLOCKTYPE, &time);
	executionStarted = time.tv_sec * 1000000000L + time.tv_nsec;

	ExecutorRunNestLevel++;

	PG_TRY();
	{
		if (prev_ExecutorFinish_hook)
			(*prev_ExecutorFinish_hook) (queryDesc);
		else
			standard_ExecutorFinish(queryDesc);
	}
	PG_FINALLY();
	{
		uint64		timeDiff;

		ExecutorRunNestLevel--;

		clock_gettime(CLOCKTYPE, &time);
		timeDiff = time.tv_sec * 1000000000L + time.tv_nsec - executionStarted;
		if (writeMode == TEXT_WRITE_MODE)
			TracePrintf("TRACE. Execution finished for %lu nanosec\n", timeDiff);
		else
			TracePrintf("\n],\n\"executionTime\": \"%lu nanosec\"\n", timeDiff);

		AfterExecution(queryDesc, planCopy);
		isFirstNodeCall = false;
	}
	PG_END_TRY();
}

static void
TraceSessionExecutorProcessUtility(PlannedStmt *pstmt,
								   const char *queryString,
								   bool readOnlyTree,
								   ProcessUtilityContext context,
								   ParamListInfo params,
								   QueryEnvironment *queryEnv,
								   DestReceiver *dest,
								   QueryCompletion *qc)
{
	char	   *planCopy;
	struct timespec time;
	uint64		executionStarted;
	FetchStmt	*fetch;
	Portal		portal;
	QueryDesc	*queryDesc;

	if (nodeTag(pstmt->utilityStmt) != T_FetchStmt)
	{
		CallOriginalExecutorFinish(pstmt, queryString, readOnlyTree,
								   context, params, queryEnv,
								   dest, qc);

		return;
	}

	fetch = (FetchStmt*) pstmt->utilityStmt;
	portal = GetPortalByName(fetch->portalname);

	if (!PortalIsValid(portal) || portal->queryDesc == NULL)
	{
		CallOriginalExecutorFinish(pstmt, queryString, readOnlyTree,
								   context, params, queryEnv,
								   dest, qc);
		return;
	}

	queryDesc = portal->queryDesc;

	if (writeMode == JSON_WRITE_MODE && !isFirstNodeCall)
	{
		TracePrintf(",\n");
	}

	planCopy = BeforeExecution(queryDesc);

	clock_gettime(CLOCKTYPE, &time);
	executionStarted = time.tv_sec * 1000000000L + time.tv_nsec;

	ExecutorRunNestLevel++;

	PG_TRY();
	{
		CallOriginalExecutorFinish(pstmt, queryString, readOnlyTree,
								   context, params, queryEnv,
								   dest, qc);
	}
	PG_FINALLY();
	{
		uint64		timeDiff;

		ExecutorRunNestLevel--;

		clock_gettime(CLOCKTYPE, &time);
		timeDiff = time.tv_sec * 1000000000L + time.tv_nsec - executionStarted;
		if (writeMode == TEXT_WRITE_MODE)
			TracePrintf("TRACE. Execution finished for %lu nanosec\n", timeDiff);
		else
			TracePrintf("\n],\n\"executionTime\": \"%lu nanosec\"\n", timeDiff);

		AfterExecution(queryDesc, planCopy);
		isFirstNodeCall = false;
	}
	PG_END_TRY();
}


/* find the plan field in explain jsonb */
static JsonbValue *
FindField(JsonbValue *json, char *field, size_t len)
{
	JsonbPair  *pairs = json->val.object.pairs;

	Assert(json->type == jbvObject);

	for (int i = 0; i < json->val.object.nPairs; i++)
	{
		if (len != (size_t) pairs[i].key.val.string.len)
			continue;
		if (!strncmp(pairs[i].key.val.string.val, field, len))
		{
			return &pairs[i].value;
		}
	}
	return NULL;
}


static void
AddExplainJsonbToNode(JsonbValue *planNode, UprobeList *strNodes)
{
	Jsonb	   *jsonb = JsonbValueToJsonb(planNode);
	char	   *stringJsonb = JsonbToCString(NULL, &jsonb->root, VARSIZE(jsonb));

	ListAdd(strNodes, stringJsonb);
	pfree(jsonb);
}


static void
JsonbDeleteField(JsonbValue *json, JsonbValue *value)
{
	JsonbPair  *pairs = json->val.object.pairs;
	int			nPairs = json->val.object.nPairs;

	Assert(json->type == jbvObject);

	for (int i = 0; i < nPairs; i++)
	{
		if (&pairs[i].value == value)
		{
			json->val.object.nPairs--;
			if (i == nPairs - 1)
				break;
			memmove(&pairs[i], &pairs[i + 1], sizeof(JsonbPair) * (nPairs - i - 1));
			break;
		}
	}
}


static void
MakeNodesRecursive(JsonbValue *planNode, UprobeList *strNodes)
{
	JsonbValue *subPlanNodes = FindField(planNode, "Plans", sizeof("Plans") - 1);
	JsonbValue	subPlans;

	if (subPlanNodes != NULL)
	{
		subPlans = *subPlanNodes;
		JsonbDeleteField(planNode, subPlanNodes);
	}
	else
	{
		subPlans.val.array.nElems = 0;
	}
	AddExplainJsonbToNode(planNode, strNodes);

	if (subPlanNodes == NULL)
		return;
	for (int i = 0; i < subPlans.val.array.nElems; i++)
	{
		MakeNodesRecursive(&subPlans.val.array.elems[i], strNodes);
	}
}


static UprobeList *
MakeNodesPlanStringsJson(char *explainString, size_t len)
{
	JsonbValue *explain = jsonToJsonbValue(explainString, len);
	JsonbValue *planNode;
	UprobeList *result;

	if (explain == NULL)
		return NULL;

	planNode = FindField(explain, "Plan", sizeof("Plan") - 1);
	if (!planNode)
		return NULL;
	ListInit(&result, (CompareFunction) strcmp, CurrentMemoryContext);
	MakeNodesRecursive(planNode, result);
	return result;
}


static UprobeList *
MakeNodesPlanStringsText(char *startPlanExplain, char *endPlanExplain)
{
	char	   *currentNodeExplain = startPlanExplain;
	UprobeList *result;

	ListInit(&result, (CompareFunction) strcmp, CurrentMemoryContext);
	while (true)
	{
		char	   *nextNodeSymbol = strstr(currentNodeExplain, "->");
		char	   *CTESymbol = strstr(currentNodeExplain, "CTE");

		/* +4 for skipping "CTE " */
		if (CTESymbol != NULL && CTESymbol < nextNodeSymbol && strncmp(CTESymbol + 4, "Scan on", sizeof("Scan on") - 1))
		{
			CTESymbol[-1] = '\0';
			ListAdd(result, currentNodeExplain);
			/* we skip next node symbol */
			nextNodeSymbol[0] = ' ';
			nextNodeSymbol[1] = ' ';
			/* +4 for skipping"CTE " */
			currentNodeExplain = CTESymbol + 4;
			continue;
		}
		if (!nextNodeSymbol || nextNodeSymbol >= endPlanExplain)
		{
			break;
		}
		nextNodeSymbol[0] = '\0';
		ListAdd(result, currentNodeExplain);
		currentNodeExplain = nextNodeSymbol + 4;
	}
	endPlanExplain[0] = '\0';
	ListAdd(result, currentNodeExplain);
	return result;
}
