#include "postgres.h"
#include <time.h>

#include "trace_lock_on_buffers.h"
#include "trace_session.h"
#include "trace_file.h"

#include "trace_parsing.h"

typedef struct ParsingTrace
{
	/* using 0 as invalid val */
	uint64		parsingTime;
	HTAB	   *bufferLocksStat;
	uint64		startTime;
} ParsingTrace;


static ParsingTrace parsingTrace =
{
	.startTime = 0,.bufferLocksStat = 0,.parsingTime = 0
};

static void ParsingTraceInFunc(void *data);
static void ParsingTraceRetFunc(void *data);
static void ParsingTraceCleanFunc(UprobeAttachInterface *uprobe);


static void
ParsingTraceInFunc(void *data)
{
	struct timespec time;

	LockOnBuffersTraceStatPush();

	clock_gettime(CLOCK_MONOTONIC, &time);

	parsingTrace.startTime = time.tv_sec * 1000000000L + time.tv_nsec;
}


static void
ParsingTraceRetFunc(void *data)
{
	struct timespec time;

	clock_gettime(CLOCK_MONOTONIC, &time);

	parsingTrace.parsingTime = time.tv_sec * 1000000000L + time.tv_nsec - parsingTrace.startTime;

	if (parsingTrace.bufferLocksStat)
		hash_destroy(parsingTrace.bufferLocksStat);

	parsingTrace.bufferLocksStat = LockOnBuffersTraceStatPopAndGet();
}


static void
ParsingTraceCleanFunc(UprobeAttachInterface *uprobe)
{
	pfree(uprobe);
}


UprobeAttachInterface *
ParsingUprobeGet(void)
{
	UprobeAttachInterface *res = (UprobeAttachInterface *) palloc0(sizeof(UprobeAttachInterface));

	res->cleanFunc = ParsingTraceCleanFunc;
	res->inFunc = ParsingTraceInFunc;
	res->numArgs = 0;
	res->retFunc = ParsingTraceRetFunc;
	res->targetSymbol = "raw_parser";
	return res;
}


void
ParsingWriteData(void)
{
	if (parsingTrace.parsingTime == 0)
		return;

	if (writeMode == TEXT_WRITE_MODE)
		TracePrintf("TRACE PARSE. parsingTime %lu nanosec\n", parsingTrace.parsingTime);
	else
		TracePrintf("\n\"parsingTime\": \"%lu nanosec\",\n", parsingTrace.parsingTime);

	LockOnBuffersTraceWriteStatWithName(parsingTrace.bufferLocksStat, "LWLockParsing");

	ParsingClearData();
}

void
ParsingClearData(void)
{
	if (parsingTrace.bufferLocksStat)
		hash_destroy(parsingTrace.bufferLocksStat);

	parsingTrace.bufferLocksStat = NULL;
	parsingTrace.parsingTime = 0;
}
