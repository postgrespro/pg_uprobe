#include "postgres.h"
#include <time.h>
#include "miscadmin.h"
#include "utils/hsearch.h"
#include "utils/relcache.h"
#include "utils/rel.h"
#include "utils/lsyscache.h"
#if PG_MAJORVERSION_NUM > 15
#include "utils/relfilenumbermap.h"
#else
#include "utils/relfilenodemap.h"
#endif
#include "commands/dbcommands.h"
#include "commands/tablespace.h"
#include "storage/buf_internals.h"
#include "storage/lwlock.h"

#include "trace_wait_events.h"
#include "trace_session.h"
#include "trace_file.h"
#include "list.h"

#include "trace_lock_on_buffers.h"

typedef struct BufferLWLockStatData
{
	BufferTag	bufferTag;
	int			lastCallMode;

	uint64		totalCallsExclusive;
	uint64		sleepCountExclusive;
	uint64		sleepTimeSumExclusive;
	uint64		maxSleepTimeExclusive;

	uint64		totalCallsShared;
	uint64		sleepCountShared;
	uint64		sleepTimeSumShared;
	uint64		maxSleepTimeShared;

	uint64		sleepStart;
} BufferLWLockStatData;



static BufferLWLockStatData *currentTraceLock = NULL;


static UprobeList *bufferLockStatStorageList = NULL;

static MemoryContext traceMemoryContext;

static bool isLogOnlySleep = false;

static int	BufferLockStatStorageCmp(const void *htab1, const void *htab2);
static HTAB *BufferLWLockStatStorageInit(void);
static void BufferLWLockStatStorageDelete(void);
static void BufferLWLockTraceInFunc(void *data, Buffer buffer, int mode);
static void BufferLWLockTraceRetFunc(void);
static void BufferLWLockTraceClean(UprobeAttachInterface *uprobe);

static void LWLockTraceSleepInFunc(void);
static void LWLockTraceSleepRetFunc(void);
static void LWLockTraceSleepClean(UprobeAttachInterface *uprobe);

static void LockOnBuffersTraceStartWrite(StringInfo stream);
static void LockOnBuffersTraceEndWrite(StringInfo stream, bool resultIsEmpty);
static bool LockOnBuffersTraceWriteOneLock(StringInfo stream, BufferLWLockStatData *lwlockData);
static bool LockOnBuffersTraceWriteStatInternal(HTAB *statStorage, StringInfo stream, bool shouldClean);


static int
BufferLockStatStorageCmp(const void *htab1, const void *htab2)
{
	return htab1 == htab2 ? 0 : 1;
}


static HTAB *
BufferLWLockStatStorageInit(void)
{
	HASHCTL		map_info;

	map_info.keysize = sizeof(BufferTag);
	map_info.entrysize = sizeof(BufferLWLockStatData);
	map_info.match = memcmp;
	return hash_create("map for BufferLWLock trace", 128, &map_info, HASH_ELEM | HASH_COMPARE | HASH_BLOBS);
}


static void
BufferLWLockStatStorageDelete(void)
{
	LIST_FOREACH(bufferLockStatStorageList, it)
	{
		hash_destroy(it->value);
	}
	ListFree(bufferLockStatStorageList);
}


static void
BufferLWLockTraceInFunc(void *data, Buffer buffer, int mode)
{
	bool		isFound;
	BufferDesc *desc;
	HTAB	   *currentStorage = (HTAB *) LIST_LAST(bufferLockStatStorageList);

	if (BufferIsLocal(buffer) || mode == BUFFER_LOCK_UNLOCK)
		return;
	Assert(currentStorage != NULL);
	desc = GetBufferDescriptor(buffer - 1);
	if (CritSectionCount == 0)
	{
		currentTraceLock = hash_search(currentStorage, &desc->tag, HASH_ENTER, &isFound);
	}
	else
	{
		currentTraceLock = hash_search(currentStorage, &desc->tag, HASH_FIND, &isFound);
		if (currentTraceLock == NULL)
			return;
	}
		currentTraceLock->lastCallMode = mode;
	if (!isFound)
	{
		currentTraceLock->sleepCountExclusive = 0;
		currentTraceLock->sleepTimeSumExclusive = 0;
		currentTraceLock->totalCallsExclusive = 0;
		currentTraceLock->maxSleepTimeExclusive = 0;

		currentTraceLock->sleepCountShared = 0;
		currentTraceLock->sleepTimeSumShared = 0;
		currentTraceLock->totalCallsShared = 0;
		currentTraceLock->maxSleepTimeShared = 0;
	}
	if (mode == BUFFER_LOCK_SHARE)
	{
		currentTraceLock->totalCallsShared++;
	}
	else if (mode == BUFFER_LOCK_EXCLUSIVE)
	{
		currentTraceLock->totalCallsExclusive++;
	}
}


static void
BufferLWLockTraceRetFunc(void)
{
	currentTraceLock = NULL;
}


static void
BufferLWLockTraceClean(UprobeAttachInterface *uprobe)
{
	pfree(uprobe);
	BufferLWLockStatStorageDelete();
}


static void
LWLockTraceSleepInFunc(void)
{
	struct timespec time;
	uint64		timeNano;

	clock_gettime(CLOCK_MONOTONIC, &time);
	timeNano = time.tv_sec * 1000000000L + time.tv_nsec;
	SignalWaitEventStart(timeNano);

	if (!currentTraceLock)
		return;

	if (currentTraceLock->lastCallMode == LW_SHARED)
		currentTraceLock->sleepCountShared++;
	else
		currentTraceLock->sleepCountExclusive++;

	currentTraceLock->sleepStart = timeNano;
}


static void
LWLockTraceSleepRetFunc(void)
{
	struct timespec time;
	uint64		timeNano;
	uint64		sleepTime;

	clock_gettime(CLOCK_MONOTONIC, &time);
	timeNano = time.tv_sec * 1000000000L + time.tv_nsec;
	SignalWaitEventEnd(timeNano);

	if (!currentTraceLock)
		return;

	sleepTime = timeNano - currentTraceLock->sleepStart;
	if (currentTraceLock->lastCallMode == LW_SHARED)
	{
		currentTraceLock->sleepTimeSumShared += sleepTime;
		if (sleepTime > currentTraceLock->maxSleepTimeShared)
			currentTraceLock->maxSleepTimeShared = sleepTime;
	}
	else
	{
		currentTraceLock->sleepTimeSumExclusive += sleepTime;
		if (sleepTime > currentTraceLock->maxSleepTimeExclusive)
			currentTraceLock->maxSleepTimeExclusive = sleepTime;
	}
}


static void
LWLockTraceSleepClean(UprobeAttachInterface *uprobe)
{
	pfree(uprobe);
}


/* return 2 Uprobes to attach in resUrpobesToAttach array */
void
LockOnBuffersUprobesGet(MemoryContext context, UprobeAttachInterface **resUrpobesToAttach, bool shouldLogOnlySleep)
{
	UprobeAttachInterface *uprobe = palloc0(sizeof(UprobeAttachInterface));

	uprobe->cleanFunc = BufferLWLockTraceClean;
	uprobe->inFunc = BufferLWLockTraceInFunc;
	uprobe->retFunc = BufferLWLockTraceRetFunc;
	uprobe->numArgs = 2;
	uprobe->targetSymbol = "LockBuffer";

	resUrpobesToAttach[0] = uprobe;

	uprobe = palloc0(sizeof(UprobeAttachInterface));
	uprobe->cleanFunc = LWLockTraceSleepClean;
	uprobe->inFunc = LWLockTraceSleepInFunc;
	uprobe->retFunc = LWLockTraceSleepRetFunc;
	uprobe->targetSymbol = "PGSemaphoreLock";

	resUrpobesToAttach[1] = uprobe;
	ListInit(&bufferLockStatStorageList, BufferLockStatStorageCmp, context);
	ListAdd(bufferLockStatStorageList, BufferLWLockStatStorageInit());
	traceMemoryContext = context;
	isLogOnlySleep = shouldLogOnlySleep;
}


static void
LockOnBuffersTraceStartWrite(StringInfo stream)
{
	if (writeMode == JSON_WRITE_MODE)
		appendStringInfo(stream, "[\n");
}


static void
LockOnBuffersTraceEndWrite(StringInfo stream, bool resultIsEmpty)
{
	if (writeMode == JSON_WRITE_MODE && !resultIsEmpty)
	{
		/* delete last ',' in array */
		stream->data[stream->len - 2] = ' ';
		appendStringInfo(stream, "]\n");
	}
}


/*  returns true if lwlock stat was written */
static bool
LockOnBuffersTraceWriteOneLock(StringInfo stream, BufferLWLockStatData *lwlockData)
{
	Oid			relIdForTag;
	Relation	relForTag;
	char	   *spcName;
	char	   *dbName;
	char	   *namespaceName;

#if PG_MAJORVERSION_NUM > 15
	relIdForTag = RelidByRelfilenumber(lwlockData->bufferTag.spcOid, lwlockData->bufferTag.relNumber);
#else
	relIdForTag = RelidByRelfilenode(lwlockData->bufferTag.rnode.spcNode, lwlockData->bufferTag.rnode.relNode);
#endif
	if (InvalidOid == relIdForTag)
		return false;
	relForTag = RelationIdGetRelation(relIdForTag);
	if (relForTag == NULL)
		return false;
	namespaceName = get_namespace_name_or_temp(relForTag->rd_rel->relnamespace);
#if PG_MAJORVERSION_NUM > 15
	dbName = get_database_name(lwlockData->bufferTag.dbOid);
	spcName = get_tablespace_name(lwlockData->bufferTag.spcOid);
#else
	dbName = get_database_name(lwlockData->bufferTag.rnode.dbNode);
	spcName = get_tablespace_name(lwlockData->bufferTag.rnode.spcNode);
#endif
	if (writeMode == JSON_WRITE_MODE)
	{
		appendStringInfo(stream,
						 "    {\n"
						 "        \"bufferTag\": {\n"
						 "            \"spcOid\": %u,\n"
						 "            \"spcName\": \"%s\", \n"
						 "            \"dbOid\": %u,\n"
						 "            \"dbName\": \"%s\",\n"
						 "            \"relNumber\": %u,\n"
						 "            \"relName\": \"%s.%s\",\n"
						 "            \"relKind\": \"%c\",\n"
						 "            \"forkName\": \"%s\",\n"
						 "            \"blockNumber\": %u\n"
						 "        },\n"
						 "        \"exclusive\": {\n"
						 "            \"totalCalls\": %lu,\n"
						 "            \"sleepCount\": %lu,\n"
						 "            \"sleepTimeSum\": \"%lu nanosec\",\n"
						 "            \"maxSleepTime\": \"%lu nanosec\"\n"
						 "        },\n"
						 "        \"shared\": {\n"
						 "            \"totalCalls\": %lu,\n"
						 "            \"sleepCount\": %lu,\n"
						 "            \"sleepTimeSum\": \"%lu nanosec\",\n"
						 "            \"maxSleepTime\": \"%lu nanosec\"\n"
						 "        }\n"
						 "    },\n",
#if PG_MAJORVERSION_NUM > 15
						 lwlockData->bufferTag.spcOid,
						 spcName,
						 lwlockData->bufferTag.dbOid,
						 dbName,
						 lwlockData->bufferTag.relNumber,
#else
						 lwlockData->bufferTag.rnode.spcNode,
						 spcName,
						 lwlockData->bufferTag.rnode.dbNode,
						 dbName,
						 lwlockData->bufferTag.rnode.spcNode,
#endif
						 namespaceName,
						 RelationGetRelationName(relForTag),
						 relForTag->rd_rel->relkind,
						 forkNames[lwlockData->bufferTag.forkNum],
						 lwlockData->bufferTag.blockNum,
						 lwlockData->totalCallsExclusive,
						 lwlockData->sleepCountExclusive,
						 lwlockData->sleepTimeSumExclusive,
						 lwlockData->maxSleepTimeExclusive,
						 lwlockData->totalCallsShared,
						 lwlockData->sleepCountShared,
						 lwlockData->sleepTimeSumShared,
						 lwlockData->maxSleepTimeShared
			);
	}
	else
	{
		appendStringInfo(stream,
						 "BufferTag: "
						 "spcOid=%u spcName=%s "
						 "dbOid=%u dbName=%s "
						 "relNumber=%u relName=%s.%s "
						 "relKind=%c forkName=%s "
						 "blockNumber=%u\n"
						 "Exclusive: "
						 "totalCalls=%lu sleepCount=%lu "
						 "sleepTimeSum=%lu nanosec maxSleepTime=%lu nanosec\n"
						 "Shared: "
						 "totalCalls=%lu sleepCount=%lu "
						 "sleepTimeSum=%lu nanosec maxSleepTime=%lu nanosec\n\n",
#if PG_MAJORVERSION_NUM > 15
						 lwlockData->bufferTag.spcOid,
						 spcName,
						 lwlockData->bufferTag.dbOid,
						 dbName,
						 lwlockData->bufferTag.relNumber,
#else
						 lwlockData->bufferTag.rnode.spcNode,
						 spcName,
						 lwlockData->bufferTag.rnode.dbNode,
						 dbName,
						 lwlockData->bufferTag.rnode.spcNode,
#endif
						 namespaceName,
						 RelationGetRelationName(relForTag),
						 relForTag->rd_rel->relkind,
						 forkNames[lwlockData->bufferTag.forkNum],
						 lwlockData->bufferTag.blockNum,
						 lwlockData->totalCallsExclusive,
						 lwlockData->sleepCountExclusive,
						 lwlockData->sleepTimeSumExclusive,
						 lwlockData->maxSleepTimeExclusive,
						 lwlockData->totalCallsShared,
						 lwlockData->sleepCountShared,
						 lwlockData->sleepTimeSumShared,
						 lwlockData->maxSleepTimeShared
			);
	}

	RelationClose(relForTag);
	if (namespaceName)
		pfree(namespaceName);
	if (dbName)
		pfree(dbName);
	if (spcName)
		pfree(spcName);
	return true;
}


static bool
LockOnBuffersTraceWriteStatInternal(HTAB *statStorage, StringInfo stream, bool shouldClean)
{
	HASH_SEQ_STATUS mapIterator;
	BufferLWLockStatData *mapEntry;
	bool		resultIsEmpty = true;

	if (statStorage == NULL)
		return !resultIsEmpty;


	LockOnBuffersTraceStartWrite(stream);
	hash_seq_init(&mapIterator, statStorage);
	mapEntry = (BufferLWLockStatData *) hash_seq_search(&mapIterator);
	while (mapEntry)
	{
		if (isLogOnlySleep && !mapEntry->sleepCountExclusive && !mapEntry->sleepCountShared)
		{
			mapEntry = (BufferLWLockStatData *) hash_seq_search(&mapIterator);
			continue;
		}

		resultIsEmpty = !LockOnBuffersTraceWriteOneLock(stream, mapEntry);

		mapEntry = (BufferLWLockStatData *) hash_seq_search(&mapIterator);
	}

	LockOnBuffersTraceEndWrite(stream, resultIsEmpty);

	if (shouldClean)
	{
		hash_destroy(ListPopLast(bufferLockStatStorageList));
		ListAdd(bufferLockStatStorageList, BufferLWLockStatStorageInit());
	}
	return !resultIsEmpty;
}


bool
LockOnBuffersTraceWriteStat(StringInfo stream, bool shouldClean)
{
	return LockOnBuffersTraceWriteStatInternal(
											   (HTAB *) LIST_LAST(bufferLockStatStorageList), stream, shouldClean);
}


void
LockOnBuffersTraceStatPush(void)
{
	ListAdd(bufferLockStatStorageList, BufferLWLockStatStorageInit());
}

void
LockOnBuffersTraceStatPop(void)
{
	hash_destroy(ListPopLast(bufferLockStatStorageList));
}


HTAB *
LockOnBuffersTraceStatPopAndGet(void)
{
	return (HTAB *) ListPopLast(bufferLockStatStorageList);
}


void
LockOnBuffersTraceWriteStatWithName(HTAB *data, const char *shortName)
{
	MemoryContext old;
	StringInfoData str;
	bool		hasLWLockStat;

	old = MemoryContextSwitchTo(traceMemoryContext);
	initStringInfo(&str);
	hasLWLockStat = LockOnBuffersTraceWriteStatInternal(data, &str, false);
	if (hasLWLockStat)
	{
		if (writeMode == TEXT_WRITE_MODE)
			TracePrintf("TRACE LWLOCK. %s: %s", shortName, str.data);
		else
			TracePrintf(
						"\"%s\": %s,\n",
						shortName,
						str.data
				);
	}
	pfree(str.data);
	MemoryContextSwitchTo(old);
}
