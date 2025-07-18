#include "postgres.h"
#include <time.h>
#include "utils/memutils.h"
#include "utils/portal.h"
#include "utils/jsonb.h"
#include "utils/guc.h"
#include "commands/explain.h"
#include "storage/ipc.h"

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


static UprobeList* uprobesList = NULL;

static MemoryContext traceMemoryContext = NULL;

static uint64 executionStarted;

static volatile Uprobe* lastSetUprobe = NULL; //we need it in case that ListAdd fails and we need to delete this uprobe

static ExecutorRun_hook_type prev_ExecutorRun_hook = NULL; // hook to log plan before execution

bool isExecuteTime = false;

//GUC parametrs
int writeMode = JSON_WRITE_MODE;
bool traceLWLocksForEachNode;
bool writeOnlySleepLWLocksStat;


static const struct config_enum_entry writeModeOptions[] = {
    {"text", TEXT_WRITE_MODE, false},
    {"json", JSON_WRITE_MODE, false},
    {NULL, 0, false}
};

static void TraceSessionExecutorRun(QueryDesc *queryDesc,
                                       ScanDirection direction,
                                       uint64 count,
                                       bool execute_once);
static char* ProcessDescBeforeExec(QueryDesc *queryDesc);

static UprobeList* MakeNodesPlanStringsText(char* startPlanExplain, char* endPlanExplain);

static JsonbValue* FindField(JsonbValue* json, char* field, size_t len);
static void JsonbDeleteField(JsonbValue* json, JsonbValue* value);
static void AddExplainJsonbToNode(JsonbValue* planNode, UprobeList* strNodes);
static void MakeNodesRecursive(JsonbValue* planNode, UprobeList* strNodes);
static UprobeList* MakeNodesPlanStringsJson(char* explainString, size_t len);

static void SetUprobes2Ptr(UprobeAttachInterface** uprobes, size_t size);
static void SetUprobes1Ptr(UprobeAttachInterface* uprobes, size_t size);

static char* BeforeExecution(QueryDesc *queryDesc);
static void AfterExecution(QueryDesc* queryDesc, char* planString);

static void ClearAfterEreportInFunc(void* data);
static void ClearAfterEreportRetFunc(void* data);
static void ClearAfterEreportCleanFunc(UprobeAttachInterface* uprobe);
static UprobeAttachInterface* UprobeOnClearAfterEreportGet(void);


static void
SetUprobes2Ptr(UprobeAttachInterface** uprobes, size_t size)
{
    Uprobe* uprobe;
    UPROBE_INIT_RES res;
    for (size_t i = 0; i < size; i++)
    {
        res = UprobeInit(uprobes[i], &uprobe);
        if (res != SUCCESS)
        {
            elog(ERROR, "can't set uprobe for %s symbol", uprobes[i]->targetSymbol);
        }
        lastSetUprobe = uprobe; //save it in case ListAdd faild
        ListAdd(uprobesList, uprobe);
        lastSetUprobe = NULL;
    }
}


static void
SetUprobes1Ptr(UprobeAttachInterface* uprobes, size_t size)
{
    Uprobe* uprobe;
    UPROBE_INIT_RES res;
    for (size_t i = 0; i < size; i++)
    {
        res = UprobeInit(&uprobes[i], &uprobe);
        if (res != SUCCESS)
        {
            elog(ERROR, "can't set uprobe for %s symbol", uprobes[i].targetSymbol);
        }
        lastSetUprobe = uprobe; //save it in case ListAdd faild
        ListAdd(uprobesList, uprobe);
        lastSetUprobe = NULL;
    }
}


static char*
BeforeExecution(QueryDesc *queryDesc)
{
    struct timespec time;
    char* planCopy;
    clock_gettime(CLOCKTYPE, &time);
    executionStarted = time.tv_sec * 1000000000L + time.tv_nsec;
    LockOnBuffersTraceStatPush();
    TraceWaitEventsClearStat();

    planCopy = ProcessDescBeforeExec(queryDesc);
    isExecuteTime = true;
    if (writeMode == JSON_WRITE_MODE)
        TracePrintf("\"executionEvents\": [\n");

    return planCopy;
}


static void
AfterExecution(QueryDesc* queryDesc, char* planString)
{
    struct timespec time;
    uint64 timeDiff;
    StringInfoData str;
    MemoryContext old;
    bool hasStat;
    clock_gettime(CLOCKTYPE, &time);

    timeDiff = time.tv_sec * 1000000000L + time.tv_nsec - executionStarted;
    if (writeMode == TEXT_WRITE_MODE)
        TracePrintf("TRACE. Execution finished for %lu nanosec\n", timeDiff);
    else
        TracePrintf("\n],\n\"executionTime\": \"%lu nanosec\"\n", timeDiff);

    isExecuteTime = false;
    if (!traceMemoryContext)
    {
        if (writeMode == JSON_WRITE_MODE)
        {
            TracePrintf("\n}]");
        }
        CloseTraceSessionFile();
        return;
    }

    ExecutorTraceDumpAndClearStat(queryDesc, planString);

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
        TracePrintf("},\n");
}


static void
ClearAfterEreportInFunc(void* data)
{
    ExecutorTraceClearStat();
    LockOnBuffersTraceClearStat();
}


static void
ClearAfterEreportRetFunc(void* data)
{

}


static void
ClearAfterEreportCleanFunc(UprobeAttachInterface* uprobe)
{
    pfree(uprobe);
}


static UprobeAttachInterface*
UprobeOnClearAfterEreportGet(void)
{
    UprobeAttachInterface* res = palloc0(sizeof(UprobeAttachInterface));
    res->cleanFunc = ClearAfterEreportCleanFunc;
    res->inFunc = ClearAfterEreportInFunc;
    res->retFunc = ClearAfterEreportRetFunc;
    res->targetSymbol = "FlushErrorState";
    return res;
}


static void
TraceNotNormalShutdown(void)
{
    if (!traceFile)
        return;
    if (writeMode == JSON_WRITE_MODE)
        TracePrintf("{}\n]");

    CloseTraceSessionFile();
}

void
SessionTraceStart(void)
{
    UprobeAttachInterface* fixedUprobes[2];
    UprobeAttachInterface* uprobes;
    MemoryContext old;
    size_t size;
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

        PlanningUprobesGet(fixedUprobes);
        SetUprobes2Ptr(fixedUprobes, 2);

        ExecutorTraceUprobesGet(fixedUprobes, traceMemoryContext, traceLWLocksForEachNode);
        SetUprobes2Ptr(fixedUprobes, 1);

        uprobes = UprobeOnClearAfterEreportGet();
        SetUprobes1Ptr(uprobes, 1);

        uprobes = TraceWaitEventsUprobesGet(&size);
        SetUprobes1Ptr(uprobes, size);

        on_proc_exit((pg_on_exit_callback) TraceNotNormalShutdown, 0);
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
            UprobeDelete((Uprobe*) lastSetUprobe);
        lastSetUprobe = NULL;
        TraceWaitEventsUprobesClean();
        PG_RE_THROW();
    }
    PG_END_TRY();

    if (writeMode == JSON_WRITE_MODE)
        TracePrintf("[\n");


    MemoryContextSwitchTo(old);
}


void
SessionTraceStop(void)
{
    if (uprobesList == NULL)
        elog(ERROR, "session trace is not ON");
    ExecutorTraceStop();
    LIST_FOREACH(uprobesList, it)
    {
        UprobeDelete(it->value);
    }
    uprobesList = NULL;
    MemoryContextDelete(traceMemoryContext);
    traceMemoryContext = NULL;
    //outputFile will be closed at TraceSessionExecutorRun
}


void
SessionTraceInit(void)
{
    prev_ExecutorRun_hook = ExecutorRun_hook;
    ExecutorRun_hook = TraceSessionExecutorRun;

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


static char*
ProcessDescBeforeExec(QueryDesc *queryDesc)
{
    MemoryContext old;
    ExplainState* es;
    size_t explainPlanStartOffset;
    size_t explainPlanEnd;
    UprobeList* stringPlanParts;
    char* planCopy;
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
        stringPlanParts = MakeNodesPlanStringsText(es->str->data + explainPlanStartOffset, es->str->data + explainPlanEnd);
    }
    else
    {
        planCopy = MemoryContextStrdup(traceMemoryContext, es->str->data);
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
TraceSessionExecutorRun(QueryDesc* queryDesc, ScanDirection direction, uint64 count, bool execute_once)
{
    bool traceWasOn = false;
    char* planCopy;
    if (traceMemoryContext)
    {
        planCopy = BeforeExecution(queryDesc);
        traceWasOn = true;
    }


    if (prev_ExecutorRun_hook)
        (*prev_ExecutorRun_hook) (queryDesc, direction, count, execute_once);
    else
        standard_ExecutorRun(queryDesc, direction, count, execute_once);

    //if it is the end of Session Trace, we should close trace file
    if (traceWasOn)
    {
        AfterExecution(queryDesc, planCopy);
    }
}


//find the plan field in explain jsonb
static JsonbValue*
FindField(JsonbValue* json, char* field, size_t len)
{
    JsonbPair* pairs = json->val.object.pairs;
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
AddExplainJsonbToNode(JsonbValue* planNode, UprobeList* strNodes)
{
    Jsonb* jsonb = JsonbValueToJsonb(planNode);
    char* stringJsonb = JsonbToCString(NULL, &jsonb->root, VARSIZE(jsonb));
    ListAdd(strNodes, stringJsonb);
    pfree(jsonb);
}


static void
JsonbDeleteField(JsonbValue* json, JsonbValue* value)
{
    JsonbPair* pairs = json->val.object.pairs;
    int nPairs = json->val.object.nPairs;
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
MakeNodesRecursive(JsonbValue* planNode, UprobeList* strNodes)
{
    JsonbValue* subPlanNodes = FindField(planNode, "Plans", sizeof("Plans") - 1);
    JsonbValue subPlans;

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


static UprobeList*
MakeNodesPlanStringsJson(char* explainString, size_t len)
{
    JsonbValue* explain = jsonToJsonbValue(explainString, len);
    JsonbValue* planNode;
    UprobeList* result;
    if (explain == NULL)
        return NULL;

    planNode = FindField(explain, "Plan", sizeof("Plan") - 1);
    if (!planNode)
        return NULL;
    ListInit(&result, (CompareFunction) strcmp, CurrentMemoryContext);
    MakeNodesRecursive(planNode, result);
    return result;
}


static UprobeList*
MakeNodesPlanStringsText(char* startPlanExplain, char* endPlanExplain)
{
    char* currentNodeExplain = startPlanExplain;
    UprobeList* result;
    ListInit(&result, (CompareFunction) strcmp, CurrentMemoryContext);
    while (true)
    {
        char* nextNodeSymbol = strstr(currentNodeExplain, "->");
        char* CTESymbol = strstr(currentNodeExplain, "CTE");
        //                                                                      +4 for skipping "CTE "
        if (CTESymbol != NULL && CTESymbol < nextNodeSymbol && strncmp(CTESymbol + 4, "Scan on", sizeof("Scan on") - 1))
        {
            CTESymbol[-1] = '\0';
            ListAdd(result, currentNodeExplain);
            //we skip next node symbol
            nextNodeSymbol[0] = ' ';
            nextNodeSymbol[1] = ' ';
            currentNodeExplain = CTESymbol + 4; //+4 for skipping "CTE "
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