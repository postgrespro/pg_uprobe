#include "postgres.h"
#include <time.h>

#include "trace_lock_on_buffers.h"
#include "trace_session.h"
#include "trace_file.h"

#include "trace_parsing.h"

typedef struct ParsingTrace
{
    uint64 startTime;
} ParsingTrace;


static ParsingTrace parsingTrace = {.startTime = 0};

static void ParsingTraceInFunc(void* data);
static void ParsingTraceRetFunc(void* data);
static void ParsingTraceCleanFunc(UprobeAttachInterface* uprobe);


static void
ParsingTraceInFunc(void* data)
{
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);

    parsingTrace.startTime = time.tv_sec * 1000000000L + time.tv_nsec;

    LockOnBuffersTraceStatPush();
}


static void
ParsingTraceRetFunc(void* data)
{
    struct timespec time;
    uint64 timeDiff;
    clock_gettime(CLOCK_MONOTONIC, &time);

    timeDiff = time.tv_sec * 1000000000L + time.tv_nsec - parsingTrace.startTime;
    if (writeMode == TEXT_WRITE_MODE)
        TracePrintf("TRACE PARSE. parsingTime %lu nanosec\n", timeDiff);
    else
        TracePrintf("{\n\"parsingTime\": \"%lu nanosec\",\n", timeDiff);

    LockOnBuffersTraceDumpCurrentStatWithPrefix("Buffer locks usage for parsing", "LWLockParsing");
    LockOnBuffersTraceStatPop();
}


static void
ParsingTraceCleanFunc(UprobeAttachInterface* uprobe)
{
    pfree(uprobe);
}


UprobeAttachInterface*
ParsingUprobeGet(void)
{
    UprobeAttachInterface* res = (UprobeAttachInterface*) palloc0(sizeof(UprobeAttachInterface));
    res->cleanFunc = ParsingTraceCleanFunc;
    res->inFunc = ParsingTraceInFunc;
    res->numArgs = 0;
    res->retFunc = ParsingTraceRetFunc;
    res->targetSymbol = "raw_parser";
    return res;
}
