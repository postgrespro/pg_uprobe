#ifndef TRACE_EXECUTE_NODES_H
#define TRACE_EXECUTE_NODES_H
#include "postgres.h"
#include "nodes/execnodes.h"

#include "list.h"
#include "uprobe_attach_interface.h"


extern bool isFirstNodeCall;

extern void ExecutorTraceUprobesGet(UprobeAttachInterface **uprobes, MemoryContext context, bool shouldTraceLWLocksForEachNode);

extern void ExecutorTraceDumpAndClearStat(QueryDesc *queryDesc, char *plan);

extern void ExecutorTraceStop(void);

extern void InitExecutorNodes(PlanState *plan, UprobeList *stringPlanParts);

extern void ExecuteNodesStateNew(QueryDesc *query);

extern void ExecuteNodesStatePush(QueryDesc *query);

extern void ExecuteNodesStatePop(void);

extern void ExecuteNodesStateDelete(QueryDesc *query);

extern void ExecuteNodesDeleteRegister(QueryDesc* query);

extern void ExecuteNodesStateClean(void);

extern bool ExecuteNodesStateNeedInit(void);

#endif							/* TRACE_EXECUTE_NODES_H */
