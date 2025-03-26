#ifndef TRACE_EXECUTE_NODES_H
#define TRACE_EXECUTE_NODES_H
#include "postgres.h"
#include "nodes/execnodes.h"

#include "list.h"
#include "uprobe_attach_interface.h"

extern void ExecutorTraceUprobesGet(UprobeAttachInterface** uprobes, MemoryContext context, bool shouldTraceLWLocksForEachNode);

extern void ExecutorTraceDumpAndClearStat(QueryDesc* queryDesc, char* plan);

extern void ExecutorTraceClearStat(void);

extern void ExecutorTraceStop(void);

extern void InitExecutorNodes(PlanState* plan, UprobeList* stringPlanParts);
#endif