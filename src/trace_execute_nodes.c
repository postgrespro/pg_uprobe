#include "postgres.h"
#include <time.h>
#include "miscadmin.h"
#include "utils/hsearch.h"
#include "utils/jsonb.h"
#include "nodes/execnodes.h"
#include "nodes/nodeFuncs.h"
#include "executor/execExpr.h"

#include "list.h"
#include "trace_lock_on_buffers.h"
#include "trace_session.h"
#include "trace_file.h"
#include "json_to_jsonbvalue_parser.h"

#include "trace_execute_nodes.h"

#if PG_MAJORVERSION_NUM < 17
static inline void
initStringInfoFromString(StringInfo str, char *data, int len)
{
    Assert(data[len] == '\0');

    str->data = data;
    str->len = len;
    str->maxlen = len + 1;
    str->cursor = 0;
}
#endif


typedef struct ExecuteNodeTraceData
{
    uint64 totalCalls;
    uint64 totalTimeSum;
    uint64 maxTime;
    ExecProcNodeMtd execProcNode;

    char* explainStr;
    NodeTag tag;
} ExecuteNodeTraceData;


typedef struct ExecuteNodeListVal
{
    void* nodePtr;
    ExecuteNodeTraceData traceData;
} ExecuteNodeListVal;

typedef struct ExprNodeTraceData
{
    uint64 totalCalls;
    uint64 totalTimeSum;
    uint64 maxTime;

    ExprStateEvalFunc evalfunc; //actual expr execute function
    NodeTag tag;
} ExprNodeTraceData;

typedef struct ExprNodeListVal
{
    void* nodePtr;
    ExprNodeTraceData traceData;
} ExprNodeListVal;

typedef struct AddStatToPlanTextWalkerState
{
    StringInfo str;
    char* currentPlanString;
}AddStatToPlanTextWalkerState;

static UprobeList* uprobeNodeData = NULL;

static bool isFirstNodeCall = true;

static UprobeList* uprobeExprData = NULL;

static MemoryContext traceMemoryContext;


static char* nodeNames[] = {
    #include "node_names.h"
};


static bool traceLWlocksForEachNode = false;

static int TraceExecutorNodesCmp(const ExecuteNodeListVal* listItem, const void* nodePtr);
static int TraceExprNodesCmp(const ExecuteNodeListVal* listItem, const void* nodePtr);
static bool InitExecutorNodesWalker(PlanState* plan, UprobeList* stringPlanParts);
static char* InsertIntoStringInfo(StringInfo str, char* data, size_t position);
static bool AddStatToPlanJsonWalker(PlanState* plan, UprobeList* jsonbPlans);
static void AddStatToPlanJson(PlanState* plan, char* planString);
static void JsonbValueAddStat(JsonbValue* jsonbNode, ExecuteNodeListVal* nodeData);
static void MakeNodesRecursive(JsonbValue* planNode, UprobeList* jsonbPlans);
static JsonbValue JsonbMakeNodeStat(ExecuteNodeListVal* nodeData);
static JsonbValue* FindField(JsonbValue* json, char* field, size_t len);
static bool AddStatToPlanTextWalker(PlanState* plan, AddStatToPlanTextWalkerState* state);
static void AddStatToPlanText(PlanState* plan, char* planString);
static void MakeTextStatStringForNode(PlanState* plan, char* buffer);
static void DeleteExecuteNodeListVal(ExecuteNodeListVal* val);
static void InitExecutorNode(PlanState* Node, char* explainString);

static void ExecInitExprInFunc(void* data);
static void ExecInitExprRetFunc(void* data, ExprState* resNode);
static void ExecInitExprCleanFunc(UprobeAttachInterface* uprobe);

static TupleTableSlot* TraceExecProcNodeHook(PlanState *node);

static Datum TraceExprNodeHook(struct ExprState *expression, struct ExprContext *econtext, bool *isNull);

static void DeleteExecuteNodeListVal(ExecuteNodeListVal* val)
{
    ExecSetExecProcNode(val->nodePtr, val->traceData.execProcNode);
    if (val->traceData.explainStr)
        pfree(val->traceData.explainStr);
    pfree(val);
}

static int
TraceExecutorNodesCmp(const ExecuteNodeListVal* listItem, const void* nodePtr)
{
    return !(listItem->nodePtr == nodePtr);
}

static int
TraceExprNodesCmp(const ExecuteNodeListVal* listItem, const void* nodePtr)
{
    return !(listItem->nodePtr == nodePtr);
}


static TupleTableSlot*
TraceExecProcNodeHook(PlanState *node)
{
    TupleTableSlot* result;
    ExecuteNodeListVal* nodeData = NULL;
    struct timespec time;
    uint64 timeDiff;
    uint64 startTime;
    StringInfoData str;
    bool hasLWLockStat;

    if (uprobeNodeData == NULL)
        return NULL;

    LIST_FOREACH(uprobeNodeData, it)
    {
        if (TraceExecutorNodesCmp(it->value, node) == 0)
        {
            nodeData = it->value;
        }
    }

    if (nodeData == NULL)
    {
        return 0;
    }

    if (traceLWlocksForEachNode && uprobeNodeData)
        LockOnBuffersTraceStatPush();

    clock_gettime(CLOCK_MONOTONIC, &time);
    startTime = time.tv_sec * 1000000000L + time.tv_nsec;
    nodeData->traceData.totalCalls++;

    //call real node
    result = nodeData->traceData.execProcNode(node);
    if (!uprobeNodeData)
        return result;

    clock_gettime(CLOCK_MONOTONIC, &time);
    timeDiff = time.tv_sec * 1000000000L + time.tv_nsec - startTime;

    if (timeDiff > nodeData->traceData.maxTime)
        nodeData->traceData.maxTime = timeDiff;

    nodeData->traceData.totalTimeSum += timeDiff;
    if (writeMode == TEXT_WRITE_MODE)
        TracePrintf("TRACE EXECUTOR NODE. Node %s EXPLAIN %s finished execution for %lu nanosec\n", nodeNames[node->type], nodeData->traceData.explainStr, timeDiff);
    else
    {
        if (!isFirstNodeCall)
            TracePrintf(",\n");
        else
            isFirstNodeCall = false;
        TracePrintf(
        "{\n"
        "    \"node\": \"%s\",\n"
        "    \"explain\": %s,\n"
        "    \"executeTime\": \"%lu nanosec\"",
        nodeNames[node->type],
        nodeData->traceData.explainStr,
        timeDiff
        );
    }
    if (!traceLWlocksForEachNode)
    {
        if (writeMode == JSON_WRITE_MODE)
            TracePrintf("\n}");
        return result;
    }

    initStringInfo(&str);
    hasLWLockStat = LockOnBuffersTraceWriteStat(&str, false);
    if (hasLWLockStat)
    {
        if (writeMode == TEXT_WRITE_MODE)
            TracePrintf("TRACE LWLOCK. %s", str.data);
        else
            TracePrintf(",\n    \"LWLockStat\": %s\n", str.data);
    }

    if (writeMode == JSON_WRITE_MODE)
        TracePrintf("\n}");

    pfree(str.data);
    LockOnBuffersTraceStatPop();

    return result;
}


static void
ExecInitExprInFunc(void* data)
{

}


static void
ExecInitExprRetFunc(void* data, ExprState* resNode)
{
    ExprNodeListVal* exprData;
    if (resNode == NULL)
        return;
    exprData = MemoryContextAlloc(traceMemoryContext, sizeof(ExprNodeListVal));
    exprData->nodePtr = resNode;
    exprData->traceData.maxTime = 0;
    exprData->traceData.totalCalls = 0;
    exprData->traceData.totalTimeSum = 0;
    exprData->traceData.evalfunc = resNode->evalfunc_private;
    exprData->traceData.tag = resNode->expr->type;
    ListAdd(uprobeExprData, exprData);
    resNode->evalfunc_private = TraceExprNodeHook;

}


static void
ExecInitExprCleanFunc(UprobeAttachInterface* uprobe)
{
    LIST_FOREACH(uprobeExprData, it)
    {
        ExprNodeListVal* val = it->value;
        ExprState* node = (ExprState*) val->nodePtr;
        node->evalfunc = val->traceData.evalfunc;
        pfree(val);
    }
    ListFree(uprobeExprData);
    pfree(uprobe);
    uprobeExprData = NULL;
}


static Datum
TraceExprNodeHook(struct ExprState *expression, struct ExprContext *econtext, bool *isNull)
{
    Datum result;
    ExprNodeListVal* exprData = NULL;
    struct timespec time;
    uint64 timeDiff;
    uint64 startTime;
    StringInfoData str;
    bool hasLWLockStat;

    if (uprobeExprData == NULL)
        return 0; //simply return NULL

    LIST_FOREACH(uprobeExprData, it)
    {
        if (TraceExprNodesCmp(it->value, expression) == 0)
        {
            exprData = it->value;
        }
    }

    if (exprData == NULL)
    {
        return 0; //simply return NULL
    }

    if (!isExecuteTime)
    {
        return exprData->traceData.evalfunc(expression, econtext, isNull);
    }

    if (traceLWlocksForEachNode && uprobeNodeData)
        LockOnBuffersTraceStatPush();

    clock_gettime(CLOCK_MONOTONIC, &time);
    startTime = time.tv_sec * 1000000000L + time.tv_nsec;
    exprData->traceData.totalCalls++;

    //call real node
    result = exprData->traceData.evalfunc(expression, econtext, isNull);
    if (!uprobeExprData)
        return result;

    clock_gettime(CLOCK_MONOTONIC, &time);
    timeDiff = time.tv_sec * 1000000000L + time.tv_nsec - startTime;

    if (timeDiff > exprData->traceData.maxTime)
        exprData->traceData.maxTime = timeDiff;

    exprData->traceData.totalTimeSum += timeDiff;

    if (writeMode == TEXT_WRITE_MODE)
        TracePrintf("TRACE EXECUTOR NODE. Expr Node %s finished execution for %lu nanosec\n",  nodeNames[exprData->traceData.tag], timeDiff);
    else
    {
        if (!isFirstNodeCall)
            TracePrintf("\n,");
        else
            isFirstNodeCall = false;

        TracePrintf("{\n"
        "    \"node\": \"%s\",\n"
        "    \"executeTime\": \"%lu nanosec\"",
        nodeNames[exprData->traceData.tag],
        timeDiff
        );
    }
    if (!traceLWlocksForEachNode)
    {
        if (writeMode == JSON_WRITE_MODE)
            TracePrintf("\n}");

        return result;
    }

    initStringInfo(&str);
    hasLWLockStat = LockOnBuffersTraceWriteStat(&str, false);
    if (hasLWLockStat)
    {
        if (writeMode == TEXT_WRITE_MODE)
            TracePrintf("TRACE LWLOCK. %s\n", str.data);
        else
            TracePrintf(",\n    \"LWLockStat\": %s\n", str.data);
    }

    if (writeMode == JSON_WRITE_MODE)
        TracePrintf("\n}");

    pfree(str.data);
    LockOnBuffersTraceStatPop();

    return result;
}


static void
InitExecutorNode(PlanState* node, char* explainString)
{
    ExecuteNodeListVal* nodeData;
    if (node == NULL)
        return;
    nodeData = MemoryContextAlloc(traceMemoryContext, sizeof(ExecuteNodeListVal));
    nodeData->traceData.execProcNode = node->ExecProcNodeReal;
    nodeData->nodePtr = node;
    nodeData->traceData.maxTime = 0;
    nodeData->traceData.totalCalls = 0;
    nodeData->traceData.totalTimeSum = 0;
    nodeData->traceData.explainStr = explainString;
    nodeData->traceData.tag = node->plan->type;
    ExecSetExecProcNode(node, TraceExecProcNodeHook);
    ListAdd(uprobeNodeData, nodeData);
}


//return 1 Uprobes to attach in uprobes array
void
ExecutorTraceUprobesGet(UprobeAttachInterface** uprobes, MemoryContext context, bool shouldTraceLWLocksForEachNode)
{
    traceMemoryContext = context;

    ListInit(&uprobeNodeData, (CompareFunction) TraceExecutorNodesCmp, traceMemoryContext);
    ListInit(&uprobeExprData, (CompareFunction) TraceExprNodesCmp, traceMemoryContext);

    uprobes[0] = MemoryContextAllocZero(context, sizeof(UprobeAttachInterface));
    uprobes[0]->cleanFunc = ExecInitExprCleanFunc;
    uprobes[0]->inFunc = ExecInitExprInFunc;
    uprobes[0]->needRetVal = true;
    uprobes[0]->retFunc = ExecInitExprRetFunc;
    uprobes[0]->targetSymbol = "ExecInitExpr";
    traceLWlocksForEachNode = shouldTraceLWLocksForEachNode;

}


void
ExecutorTraceDumpAndClearStat(QueryDesc* queryDesc, char* plan)
{
    if (writeMode == TEXT_WRITE_MODE)
    {
        AddStatToPlanText(queryDesc->planstate, plan);
        LIST_FOREACH(uprobeExprData, it)
        {
            ExprNodeListVal* val = it->value;
            TracePrintf("TRACE EXEC. Expr Node %s, %p: totalCalls %lu, totalTimeSum %lu nanosec, MaxTime %lu nanosec\n",
                nodeNames[val->traceData.tag], val->nodePtr, val->traceData.totalCalls, val->traceData.totalTimeSum, val->traceData.maxTime);
            pfree(val);
        }
    }
    else
    {
        AddStatToPlanJson(queryDesc->planstate, plan);
        TracePrintf(",\n\"exprNodeStat\": [\n");
        LIST_FOREACH(uprobeExprData, it)
        {
            ExprNodeListVal* val = it->value;
            TracePrintf(
                "   {\n"
                "    \"node\": \"%s\",\n"
                "    \"totalCalls\": %lu,\n"
                "    \"totalTimeSum\": \"%lu nanosec\",\n"
                "    \"maxTime\": \"%lu nanosec\"\n"
                "   }",
                nodeNames[val->traceData.tag], val->traceData.totalCalls, val->traceData.totalTimeSum, val->traceData.maxTime
            );
            if (it != uprobeExprData->tail)
                TracePrintf(",\n");

            pfree(val);
        }
        TracePrintf("\n]");
    }
    ListMakeEmpty(uprobeNodeData);
    ListMakeEmpty(uprobeExprData);
    isFirstNodeCall = true;
}


void
ExecutorTraceClearStat(void)
{

    LIST_FOREACH(uprobeNodeData, it)
    {
        DeleteExecuteNodeListVal(it->value);
    }
    ListMakeEmpty(uprobeNodeData);

    LIST_FOREACH(uprobeExprData, it)
    {
        pfree(it->value);
    }
    ListMakeEmpty(uprobeExprData);

    isFirstNodeCall = true;
}


void
ExecutorTraceStop(void)
{
    ExecutorTraceClearStat();
    uprobeNodeData = NULL;
}


static JsonbValue
JsonbMakeNodeStat(ExecuteNodeListVal* nodeData)
{
    JsonbParseState* state = NULL;
    JsonbValue* result = pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);
    JsonbValue tmp;
    tmp.type = jbvString;
    tmp.val.string.len = sizeof("totalCalls");
    tmp.val.string.val = "totalCalls";
    result = pushJsonbValue(&state, WJB_KEY, &tmp);
    tmp.type = jbvNumeric;
    tmp.val.numeric = int64_to_numeric((int64) nodeData->traceData.totalCalls);
    result = pushJsonbValue(&state, WJB_VALUE, &tmp);

    tmp.type = jbvString;
    tmp.val.string.len = sizeof("totalTimeSum");
    tmp.val.string.val = "totalTimeSum";
    result = pushJsonbValue(&state, WJB_KEY, &tmp);
    tmp.type = jbvNumeric;
    tmp.val.numeric = int64_to_numeric((int64) nodeData->traceData.totalTimeSum);
    result = pushJsonbValue(&state, WJB_VALUE, &tmp);

    tmp.type = jbvString;
    tmp.val.string.len = sizeof("maxTime");
    tmp.val.string.val = "maxTime";
    result = pushJsonbValue(&state, WJB_KEY, &tmp);
    tmp.type = jbvNumeric;
    tmp.val.numeric = int64_to_numeric((int64) nodeData->traceData.maxTime);
    result = pushJsonbValue(&state, WJB_VALUE, &tmp);

    result = pushJsonbValue(&state, WJB_END_OBJECT, NULL);

    return *result;
}


static void
JsonbValueAddStat(JsonbValue* jsonbNode, ExecuteNodeListVal* nodeData)
{
    int nPairs = jsonbNode->val.object.nPairs;
    JsonbPair* pairs = jsonbNode->val.object.pairs;
    pairs = repalloc(pairs, (nPairs + 1) * sizeof(JsonbPair));
    pairs[nPairs].key.type = jbvString;
    pairs[nPairs].key.val.string.val = "traceData";
    pairs[nPairs].key.val.string.len = sizeof("traceData");
    pairs[nPairs].value = JsonbMakeNodeStat(nodeData);
    pairs[nPairs].order = pairs[nPairs - 1].order + 1;


    jsonbNode->val.object.pairs = pairs;
    jsonbNode->val.object.nPairs++;
}


static bool
AddStatToPlanJsonWalker(PlanState* plan, UprobeList* jsonbPlans)
{
    ExecuteNodeListVal* val;
    JsonbValue* jsonbPlan;
    if (plan == NULL)
        return false;

    val = ListPop(uprobeNodeData, plan);
    if (val == NULL)
        return false;

    jsonbPlan = ListPopFirst(jsonbPlans);
    if (jsonbPlan != NULL)
        JsonbValueAddStat(jsonbPlan, val);

    DeleteExecuteNodeListVal(val);
    return planstate_tree_walker(plan, AddStatToPlanJsonWalker, jsonbPlans);
}


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
MakeNodesRecursive(JsonbValue* planNode, UprobeList* jsonbPlans)
{
    JsonbValue* subPlanNodes = FindField(planNode, "Plans", sizeof("Plans") - 1);

    ListAdd(jsonbPlans, planNode);

    if (subPlanNodes == NULL)
        return;

    for (int i = 0; i < subPlanNodes->val.array.nElems; i++)
    {
        MakeNodesRecursive(&subPlanNodes->val.array.elems[i], jsonbPlans);
    }
}


static void
AddStatToPlanJson(PlanState* plan, char* planString)
{
    JsonbValue* explain = jsonToJsonbValue(planString, strlen(planString));
    JsonbValue* planNode;
    UprobeList* jsonbPlans;
    Jsonb* jsonb;
    char* stringJsonb;
    if (explain == NULL)
        return;

    planNode = FindField(explain, "Plan", sizeof("Plan") - 1);
    ListInit(&jsonbPlans, (CompareFunction) NULL, CurrentMemoryContext); //comparator is unused

    MakeNodesRecursive(planNode, jsonbPlans);
    AddStatToPlanJsonWalker(plan, jsonbPlans);

    jsonb = JsonbValueToJsonb(planNode);
    stringJsonb = JsonbToCString(NULL, &jsonb->root, VARSIZE(jsonb));
    pfree(jsonb);
    TracePrintf(",\"executorNodeStatInPlan\": %s", stringJsonb);
    pfree(stringJsonb);
}


static void
MakeTextStatStringForNode(PlanState* plan, char* buffer)
{
    ExecuteNodeListVal* val;
    val = ListPop(uprobeNodeData, plan);
    if (val == NULL)
    {
        buffer[0] = '\0';
        return;
    }
    sprintf(buffer, " (totalCalls %lu, totalTimeSum %lu nanosec, MaxTime %lu nanosec) ",
        val->traceData.totalCalls, val->traceData.totalTimeSum, val->traceData.maxTime);

    DeleteExecuteNodeListVal(val);
    return;
}


static bool
AddStatToPlanTextWalker(PlanState* plan, AddStatToPlanTextWalkerState* state)
{
    char* nextNodeSymbol;
    char* CTESymbol;
    char statToInsert[128];
    MakeTextStatStringForNode(plan, statToInsert);
    state->currentPlanString = InsertIntoStringInfo(state->str, statToInsert, state->currentPlanString - state->str->data);
    nextNodeSymbol = strstr(state->currentPlanString , "->");
    CTESymbol = strstr(state->currentPlanString , "CTE");
    //                                                                      +4 for skipping "CTE "
    if (CTESymbol != NULL && CTESymbol < nextNodeSymbol && strncmp(CTESymbol + 4, "Scan on", sizeof("Scan on") - 1))
    {
        state->currentPlanString  = CTESymbol + 4; //+4 for skipping "CTE "
        return planstate_tree_walker(plan, AddStatToPlanTextWalker, state);
    }
    if (!nextNodeSymbol)
    {
        return false;
    }
    state->currentPlanString  = nextNodeSymbol + 4;
    return planstate_tree_walker(plan, AddStatToPlanTextWalker, state);
}


static void
AddStatToPlanText(PlanState* plan, char* planString)
{
    StringInfoData str;
    AddStatToPlanTextWalkerState state;
    initStringInfoFromString(&str, planString, strlen(planString));
    state.str = &str;
    state.currentPlanString = planString;
    AddStatToPlanTextWalker(plan, &state);
    TracePrintf("Plan with stat:\n %s \n", str.data);
    pfree(str.data);
}


static bool
InitExecutorNodesWalker(PlanState* plan, UprobeList* stringPlanParts)
{
    char* explainString = ListPopFirst(stringPlanParts);
    if (plan == NULL)
        return false;

    InitExecutorNode(plan, explainString ? MemoryContextStrdup(traceMemoryContext, explainString) : NULL);
    return planstate_tree_walker(plan, InitExecutorNodesWalker, stringPlanParts);
}


void
InitExecutorNodes(PlanState* plan, UprobeList* stringPlanParts)
{
    InitExecutorNodesWalker(plan, stringPlanParts);
}


static char*
InsertIntoStringInfo(StringInfo str, char* data, size_t position)
{
    size_t len = strlen(data);

    enlargeStringInfo(str, len);

    memmove(str->data + position + len,str->data + position, str->len - position + 1);

    memcpy(str->data + position, data, len);

    str->len += len;

    return str->data + position + len;
}