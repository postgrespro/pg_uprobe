#ifndef TRACE_PLANNING_H
#define TRACE_PLANNING_H

#include "uprobe_attach_interface.h"

extern void PlanningUprobesGet(UprobeAttachInterface **resUprobes, MemoryContext context);

extern void PlanningWriteData(void);

extern void PlanningClearData(void);

#endif							/* TRACE_PLANNING_H */
