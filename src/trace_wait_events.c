#include "postgres.h"
#include <time.h>
#include "miscadmin.h"
#include "utils/hsearch.h"
#include "utils/wait_event.h"

#include "uprobe_attach_interface.h"
#include "trace_session.h"

#include "trace_wait_events.h"

typedef struct WaitEventData
{
	uint32		eventId;
	uint64		count;
	uint64		timeSum;
	uint64		maxTime;
} WaitEventData;


static HTAB *waitEventsDataStorage = NULL;

static WaitEventData *currentWaitEvent;

static uint64 startWaitEventTime;

static UprobeAttachInterface *attachedUprobes = NULL;


static char *waitFuncs[] = {
	"pwrite",
	"ftruncate",
	"fsync",
/*     "PGSemaphoreLock",   conflict with tracing locks on shared buffers, signalWaitEventStart(End) are used to inform about this event */
	"pread",
	"fgets",
	"read",
	"write",
	"pwritev",
	"fdatasync",
	"pg_usleep",
#if defined(HAVE_SYNC_FILE_RANGE)
	"sync_file_range",
#endif

#ifdef HAVE_POSIX_FALLOCATE
	"posix_fallocate",
#endif

#if defined(WAIT_USE_EPOLL)
	"epoll_wait"
#elif defined(WAIT_USE_KQUEUE)
	"kevent"
#elif defined(WAIT_USE_POLL)
	"poll"
#endif
};

#define sizeofWaitFuncs sizeof(waitFuncs) / sizeof(waitFuncs[0])

static uint32 WaitEventDataHash(const void *key, Size keysize);
static int	WaitEventDataCmp(const void *key1, const void *key2, Size keysize);
static void WaitEventsDatStorageInit();
static void WaitEventsDataStorageDelete();

static void TraceWaitEventInFunc(void *data);
static void TraceWaitEventRetFunc(void *data);
static void TraceWaitEventClean(UprobeAttachInterface *uprobe);
static void TraceWaitEventWriteData(StringInfo stream, WaitEventData *data);

static uint32
WaitEventDataHash(const void *key, Size keysize)
{
	Assert(keysize == sizeof(uint32));
	return *((uint32 *) key);
}


static int
WaitEventDataCmp(const void *key1, const void *key2, Size keysize)
{
	int32		k1 = *((uint32 *) key1);
	int32		k2 = *((uint32 *) key2);

	Assert(keysize == sizeof(uint32));

	if (k1 - k2)
		return 1;
	return 0;
}


static void
WaitEventsDatStorageInit(void)
{
	HASHCTL		map_info;

	map_info.keysize = sizeof(uint32);
	map_info.entrysize = sizeof(WaitEventData);
	map_info.hash = &WaitEventDataHash;
	map_info.match = &WaitEventDataCmp;
	waitEventsDataStorage = hash_create("map for trace wait events", 1024, &map_info, HASH_ELEM | HASH_FUNCTION | HASH_COMPARE);
}


static void
WaitEventsDataStorageDelete(void)
{
	hash_destroy(waitEventsDataStorage);
	waitEventsDataStorage = NULL;
}


static void
TraceWaitEventInFunc(void *data)
{
	bool		isFound;
	struct timespec time;

	if (*my_wait_event_info == 0)
		return;

	if (CritSectionCount == 0)
	{
		currentWaitEvent = hash_search(waitEventsDataStorage, my_wait_event_info, HASH_ENTER, &isFound);
	}
	else
	{
		currentWaitEvent = hash_search(waitEventsDataStorage, my_wait_event_info, HASH_FIND, &isFound);
		if (currentWaitEvent == NULL)
			return;
	}

	if (!isFound)
	{
		currentWaitEvent->count = 1;
		currentWaitEvent->maxTime = 0;
		currentWaitEvent->timeSum = 0;
	}
	else
		currentWaitEvent->count++;

	clock_gettime(CLOCK_MONOTONIC, &time);
	startWaitEventTime = time.tv_nsec + time.tv_sec * 1000000000L;
}


static void
TraceWaitEventRetFunc(void *data)
{
	struct timespec time;
	uint64		timeDiff;

	if (!currentWaitEvent)
		return;

	clock_gettime(CLOCK_MONOTONIC, &time);
	timeDiff = time.tv_nsec + time.tv_sec * 1000000000L - startWaitEventTime;

	currentWaitEvent->timeSum += timeDiff;
	if (currentWaitEvent->maxTime < timeDiff)
		currentWaitEvent->maxTime = timeDiff;

	currentWaitEvent = NULL;
}

/*  used to tell that wait event has started if actual working function is used elsewhere */
void
SignalWaitEventStart(uint64 time)
{
	bool		isFound;

	if (*my_wait_event_info == 0)
		return;

	if (CritSectionCount == 0)
	{
		currentWaitEvent = hash_search(waitEventsDataStorage, my_wait_event_info, HASH_ENTER, &isFound);
	}
	else
	{
		currentWaitEvent = hash_search(waitEventsDataStorage, my_wait_event_info, HASH_FIND, &isFound);
		if (currentWaitEvent == NULL)
			return;
	}

	if (!isFound)
	{
		currentWaitEvent->count = 1;
		currentWaitEvent->maxTime = 0;
		currentWaitEvent->timeSum = 0;
	}
	else
		currentWaitEvent->count++;

	startWaitEventTime = time;
}

/*  used to tell that wait event has ended if actual working function is used elsewhere */
void
SignalWaitEventEnd(uint64 time)
{
	uint64		timeDiff;

	if (!currentWaitEvent)
		return;

	timeDiff = time - startWaitEventTime;

	currentWaitEvent->timeSum += timeDiff;
	if (currentWaitEvent->maxTime < timeDiff)
		currentWaitEvent->maxTime = timeDiff;
}


static void
TraceWaitEventClean(UprobeAttachInterface *uprobe)
{

}


UprobeAttachInterface *
TraceWaitEventsUprobesGet(size_t *resSize)
{
	WaitEventsDatStorageInit();
	attachedUprobes = palloc0(sizeof(UprobeAttachInterface) * sizeofWaitFuncs);
	for (int i = 0; i < sizeofWaitFuncs; i++)
	{
		attachedUprobes[i].cleanFunc = TraceWaitEventClean;
		attachedUprobes[i].inFunc = TraceWaitEventInFunc;
		attachedUprobes[i].retFunc = TraceWaitEventRetFunc;
		attachedUprobes[i].targetSymbol = waitFuncs[i];
	}
	*resSize = sizeofWaitFuncs;
	return attachedUprobes;
}


void
TraceWaitEventsUprobesClean(void)
{
	if (!attachedUprobes)
		return;
	pfree(attachedUprobes);
	WaitEventsDataStorageDelete();
}


void
TraceWaitEventsClearStat(void)
{
	WaitEventsDataStorageDelete();
	WaitEventsDatStorageInit();
}


static void
TraceWaitEventWriteData(StringInfo stream, WaitEventData *data)
{
	if (writeMode == TEXT_WRITE_MODE)
	{
		appendStringInfo(stream,
						 "name=%s count=%lu timeSum=%lu nanosec maxTime=%lu nanosec\n",
						 pgstat_get_wait_event(data->eventId),
						 data->count,
						 data->timeSum,
						 data->maxTime
			);
	}
	else
	{
		appendStringInfo(stream,
						 "    {\n"
						 "        \"name\": \"%s\",\n"
						 "        \"count\": %lu,\n"
						 "        \"timeSum\": \"%lu nanosec\",\n"
						 "        \"maxTime\": \"%lu nanosec\"\n"
						 "    },\n",
						 pgstat_get_wait_event(data->eventId),
						 data->count,
						 data->timeSum,
						 data->maxTime
			);
	}
}


bool
TraceWaitEventDumpStat(StringInfo out)
{
	HASH_SEQ_STATUS mapIterator;
	WaitEventData *mapEntry;
	bool		hasInfo = false;

	if (!waitEventsDataStorage)
		return hasInfo;

	if (writeMode == JSON_WRITE_MODE)
		appendStringInfo(out, "[\n");

	hash_seq_init(&mapIterator, waitEventsDataStorage);
	mapEntry = (WaitEventData *) hash_seq_search(&mapIterator);
	while (mapEntry)
	{
		TraceWaitEventWriteData(out, mapEntry);

		mapEntry = (WaitEventData *) hash_seq_search(&mapIterator);
		hasInfo = true;
	}
	if (writeMode == JSON_WRITE_MODE && hasInfo)
	{
		/* delete last ',' in array */
		out->data[out->len - 2] = ' ';
		appendStringInfo(out, "]\n");
	}
	WaitEventsDataStorageDelete();
	WaitEventsDatStorageInit();

	return hasInfo;
}
