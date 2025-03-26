#ifndef TRACE_LWLOCK_H
#define TRACE_LWLOCK_H
#include "postgres.h"
#include "lib/stringinfo.h"

#include "uprobe_attach_interface.h"


extern void LockOnBuffersUprobesGet(MemoryContext context, UprobeAttachInterface** resUrpobesToAttach, bool shouldLogOnlySleep);


extern bool LockOnBuffersTraceWriteStat(StringInfo stream, bool shouldClean);

extern void LockOnBuffersTraceStatPush(void);

extern void LockOnBuffersTraceStatPop(void);

extern void LockOnBuffersTraceClearStat(void);

extern void LockOnBuffersTraceDumpCurrentStatWithPrefix(const char* info, const char* shortName);
#endif