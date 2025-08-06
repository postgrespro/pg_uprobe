#ifndef TRACE_LOCK_ON_BUFFERS
#define TRACE_LOCK_ON_BUFFERS
#include "postgres.h"
#include "lib/stringinfo.h"
#include "utils/hsearch.h"

#include "uprobe_attach_interface.h"


extern void LockOnBuffersUprobesGet(MemoryContext context, UprobeAttachInterface **resUrpobesToAttach, bool shouldLogOnlySleep);


extern bool LockOnBuffersTraceWriteStat(StringInfo stream, bool shouldClean);

extern void LockOnBuffersTraceStatPush(void);

extern void LockOnBuffersTraceStatPop(void);

extern HTAB *LockOnBuffersTraceStatPopAndGet(void);

extern void LockOnBuffersTraceWriteStatWithName(HTAB *data, const char *shortName);

#endif							/* TRACE_LOCK_ON_BUFFERS */
