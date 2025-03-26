#include "postgres.h"
#include <time.h>
#include <math.h>
#include "utils/hsearch.h"
#include "utils/memutils.h"

#include "uprobe_message_buffer.h"
#include "trace_file.h"

#include "count_uprobes.h"

typedef struct UprobeAttachTimeData
{
    uint64 totalCalls;
    uint64 totalTimeSumm;
    uint64 prevCallTime;
} UprobeAttachTimeData;


typedef struct UrpobeStorageTime
{
    UprobeStorage base;

    uint64 totalCalls;
    uint64 timeSum;
} UprobeStorageTime;


typedef struct MessageTime
{
    Message base;
    uint64 numCalls;
    uint64 totalTime;
} MessageTime;


static void UprobeAttachTimeClean(UprobeAttachInterface* this);
static void UprobeAttachTimeInFunc(UprobeAttachTimeData* data);
static void UprobeAttachTimeRetFunc(UprobeAttachTimeData* data);
static void UprobeAttachTimeTimedCallback(UprobeAttachInterface* this);


typedef struct UprobeAttachHistMapEntry
{
    uint64 time; // in 10^-7 seconds
    uint64 count;
} UprobeAttachHistMapEntry;


typedef struct UprobeAttachHistData
{
    HTAB* map;
    uint64 prevCallTime;
} UprobeAttachHistData;

typedef struct MessageHist
{
    Message base;
    uint64 time;
    uint64 count;
} MessageHist;



typedef struct UprobeStorageHist
{
    UprobeStorage base;

    HTAB* map;
} UprobeStorageHist;


static uint32 UprobeMapKeyHash(const void *key, Size keysize);
static int UprobeMapKeyCmp(const void *key1, const void *key2, Size keysize);
static HTAB* CreateHashMapForHistUprobe();

static void UprobeAttachHistClean(UprobeAttachInterface* this);
static void UprobeAttachHistInFunc(UprobeAttachHistData* data);
static void UprobeAttachHistRetFunc(UprobeAttachHistData* data);
static void UprobeAttachHistTimedCallback(UprobeAttachInterface* this);



typedef struct UprobeAttachMemMapEntry
{
    int64 memory;
    uint64 count;
} UprobeAttachMemMapEntry;


typedef struct UprobeAttachMemData
{
    HTAB* map;
    int64 prevMemCount;
} UprobeAttachMemData;


typedef struct MessageMem
{
    Message base;
    int64 memory;
    uint64 count;
}MessageMem;


typedef struct UprobeStorageMem
{
    UprobeStorage base;

    HTAB* map;
} UprobeStorageMem;


static HTAB* CreateHashMapForMemUprobe();

static void UprobeAttachMemClean(UprobeAttachInterface* this);
static void UprobeAttachMemInFunc(UprobeAttachMemData* data);
static void UprobeAttachMemRetFunc(UprobeAttachMemData* data);
static void UprobeAttachMemTimedCallback(UprobeAttachInterface* this);



UprobeAttachInterface*
UprobeAttachTimeInit(const char* symbol)
{
    UprobeAttachInterface* res = palloc(sizeof(UprobeAttachInterface));
    res->inFunc = UprobeAttachTimeInFunc;
    res->retFunc = UprobeAttachTimeRetFunc;
    res->timedCallback = UprobeAttachTimeTimedCallback;
    res->cleanFunc = UprobeAttachTimeClean;
    res->needRetVal = false;
    res->numArgs = 0;
    res->data = palloc0(sizeof(UprobeAttachTimeData));
    res->targetSymbol = pstrdup(symbol);
    return res;
}


static void
UprobeAttachTimeClean(UprobeAttachInterface* this)
{
    if (this == NULL)
        return;

    pfree(this->data);
    pfree(this->targetSymbol);
    pfree(this);
}


static void
UprobeAttachTimeInFunc(UprobeAttachTimeData* data)
{
    struct timespec time;

    clock_gettime(CLOCK_MONOTONIC, &time);
    data->prevCallTime = time.tv_nsec + time.tv_sec * 1000000000L;
}


static void
UprobeAttachTimeRetFunc(UprobeAttachTimeData* data)
{
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);

    data->totalCalls++;
    data->totalTimeSumm += time.tv_nsec + time.tv_sec * 1000000000L - data->prevCallTime;
}


static void
UprobeAttachTimeTimedCallback(UprobeAttachInterface* this)
{
    UprobeAttachTimeData* data = (UprobeAttachTimeData*) this->data;
    MessageTime mes;
    mes.base.type = MESSAGE_CUSTOM;
    mes.base.size = sizeof(MessageTime);
    mes.numCalls = data->totalCalls;
    data->totalCalls = 0;
    mes.totalTime = data->totalTimeSumm;
    data->totalTimeSumm = 0;
    MessageBufferPut((Message*) &mes, 1, this->targetSymbol);
}


static void
UprobeStorageTimePutData(UprobeStorageTime* storage, void* data)
{
    MessageTime* mes = data;
    storage->totalCalls += mes->numCalls;
    storage->timeSum += mes->totalTime;
}


static void
UprobeStorageTimeWriteStat(UprobeStorageTime* storage, bool shouldClearStat)
{
    uint64 avgTime;
    FILE* file;
    char filePath[MAXPGPATH];
    sprintf(filePath,"%sTIME_%s.txt", dataDir, storage->base.symbol);
    file = fopen(filePath, "w");
    if (file == NULL)
    {
        elog(LOG, "can't open file %s for writting", filePath);
        return;
    }

    if (storage->totalCalls == 0)
        avgTime = 0;
    else
        avgTime = storage->timeSum / storage->totalCalls;

    fprintf(file, "num calls: %lu, avg time: %lu nanosec\n", storage->totalCalls, avgTime);
    fclose(file);
    if (shouldClearStat)
    {
        storage->timeSum = 0;
        storage->totalCalls = 0;
    }
}


static void
UprobeStorageTimeDelete(UprobeStorageTime* storage, bool shouldWriteStat)
{
    if (shouldWriteStat)
        UprobeStorageTimeWriteStat(storage, false);

    pfree(storage->base.symbol);
    pfree(storage);
}


UprobeStorage*
UprobeStorageTimeInit(const char* symbol)
{
    UprobeStorageTime* storage = palloc(sizeof(UprobeStorageTime));
    storage->base.delete = (StorageDeleteFunc) UprobeStorageTimeDelete;
    storage->base.putData = (StoragePutDataFunc) UprobeStorageTimePutData;
    storage->base.writeStat = (StorageWriteStat) UprobeStorageTimeWriteStat;
    storage->base.symbol = pstrdup(symbol);
    storage->timeSum = 0;
    storage->totalCalls = 0;

    return (UprobeStorage*) storage;
}


UprobeAttachTimeStat*
UprobeAttachTimeGetStat(const UprobeAttachInterface* uprobe)
{
    UprobeAttachTimeStat* res = palloc(sizeof(UprobeAttachTimeStat));
    UprobeAttachTimeData* data = uprobe->data;

    res->numCalls = data->totalCalls;
    if (data->totalCalls)
        res->avgTime = data->totalTimeSumm / data->totalCalls;
    else
        res->avgTime = 0;

    return res;
}


static uint32
UprobeMapKeyHash(const void *key, Size keysize)
{
    Assert(keysize == sizeof(uint64));
    return (uint32) *((uint64*) key);
}


static int
UprobeMapKeyCmp(const void *key1, const void *key2, Size keysize)
{
    int64 k1 = *((uint64*) key1);
    int64 k2 = *((uint64*) key2);

    Assert(keysize == sizeof(uint64));

    if (k1 - k2)
        return 1;
    return 0;
}


static HTAB*
CreateHashMapForHistUprobe()
{
    HTAB* map;
    HASHCTL map_info;
    map_info.keysize = sizeof(uint64);
    map_info.entrysize = sizeof(UprobeAttachHistMapEntry);
    map_info.hash = &UprobeMapKeyHash;
    map_info.match = &UprobeMapKeyCmp;
    map = hash_create("map for HIST uprobe", 1024, &map_info, HASH_ELEM | HASH_FUNCTION | HASH_COMPARE);
    return map;
}


UprobeAttachInterface*
UprobeAttachHistInit(const char* symbol)
{
    UprobeAttachInterface* res = palloc(sizeof(UprobeAttachInterface));
    UprobeAttachHistData* data = palloc(sizeof(UprobeAttachHistData));
    data->map = CreateHashMapForHistUprobe();
    res->inFunc = UprobeAttachHistInFunc;
    res->retFunc = UprobeAttachHistRetFunc;
    res->timedCallback = UprobeAttachHistTimedCallback;
    res->cleanFunc = UprobeAttachHistClean;
    res->needRetVal = false;
    res->numArgs = 0;
    res->data = data;
    res->targetSymbol = pstrdup(symbol);
    return res;
}



static void
UprobeAttachHistClean(UprobeAttachInterface* this)
{
    UprobeAttachHistData* data;
    if (this == NULL)
        return;
    data = (UprobeAttachHistData*) this->data;
    hash_destroy(data->map);
    pfree(data);
    pfree(this->targetSymbol);
    pfree(this);
}


static void
UprobeAttachHistInFunc(UprobeAttachHistData* data)
{
    struct timespec time;

    clock_gettime(CLOCK_MONOTONIC, &time);
    data->prevCallTime = time.tv_nsec + time.tv_sec * 1000000000L;
}


static void
UprobeAttachHistRetFunc(UprobeAttachHistData* data)
{
    struct timespec time;
    uint64 diff;
    UprobeAttachHistMapEntry* mapEntry;
    bool isFound;
    clock_gettime(CLOCK_MONOTONIC, &time);

    diff = time.tv_nsec + time.tv_sec * 1000000000L - data->prevCallTime;

    if (diff % 100 >= 50)
        diff = (diff / 100) + 1;
    else                               //we take only 10^-7 seconds and round it
        diff = (diff / 100);

    mapEntry = (UprobeAttachHistMapEntry*) hash_search(data->map, &diff, HASH_ENTER_NULL, &isFound);
    if (likely(mapEntry))
    {
        if (likely(isFound))
            mapEntry->count += 1;
        else
            mapEntry->count = 1;
    }
}


static void
UprobeAttachHistTimedCallback(UprobeAttachInterface* this)
{
    UprobeAttachHistData* data = ( UprobeAttachHistData*) this->data;
    MessageHist messages[1024];
    uint32 currentIndex = 0;
    uint32 numSend = 0;
    HASH_SEQ_STATUS mapIterator;
    UprobeAttachHistMapEntry* mapEntry;
    hash_seq_init(&mapIterator, data->map);
    mapEntry = (UprobeAttachHistMapEntry*) hash_seq_search(&mapIterator);
    while (mapEntry)
    {
        messages[currentIndex].base.type = MESSAGE_CUSTOM;
        messages[currentIndex].base.size = sizeof(MessageHist);
        messages[currentIndex].time = mapEntry->time;
        messages[currentIndex].count = mapEntry->count;
        currentIndex++;
        if (currentIndex == 1024)
        {
            numSend = MessageBufferPut((Message*) messages, currentIndex, this->targetSymbol);
            if (numSend != currentIndex)
            {
                memmove(messages, &messages[numSend], (currentIndex - numSend) * sizeof(MessageHist));
            }
            currentIndex = currentIndex - numSend;
        }
        mapEntry = (UprobeAttachHistMapEntry*) hash_seq_search(&mapIterator);
    }
    while (currentIndex != 0)
    {
        numSend = MessageBufferPut((Message*) messages, currentIndex, this->targetSymbol);
        if (numSend != currentIndex)
        {
            memmove(messages, &messages[numSend], (currentIndex - numSend) * sizeof(MessageHist));
        }
        currentIndex = currentIndex - numSend;
    }

    hash_destroy(data->map);
    data->map = CreateHashMapForHistUprobe();
}


UprobeAttachHistStat*
UprobeAttachHistGetStat(const UprobeAttachInterface* uprobe, double start, double stop, double step)
{
    UprobeAttachHistStat* result;
    HASH_SEQ_STATUS mapIterator;
    UprobeAttachHistMapEntry* mapEntry;
    UprobeAttachHistData* data = (UprobeAttachHistData*) uprobe->data;
    size_t size = (size_t) round((stop - start) / step) + 2;

    result = (UprobeAttachHistStat*) palloc0(sizeof(UprobeAttachHistStat));
    result->histArray = (size_t*) palloc0(sizeof(size_t) * size);
    result->histArraySize = size;
    result->start = start;
    result->stop = stop;
    result->step = step;
    hash_seq_init(&mapIterator, data->map);
    mapEntry = (UprobeAttachHistMapEntry*) hash_seq_search(&mapIterator);
    while (mapEntry)
    {
        double timeUs;
        size_t index;
        timeUs = ((double) mapEntry->time) / 10.0;

        if ((timeUs - start) <= 0)
            index = 0;
        else if (timeUs >= stop)
            index = size - 1;
        else
            index = (size_t) ((timeUs - start) / step) + 1;

        result->histArray[index] += mapEntry->count;
        result->totalCalls += mapEntry->count;
        mapEntry = (UprobeAttachHistMapEntry*) hash_seq_search(&mapIterator);
    }
    return result;
}


UprobeAttachHistStat*
UprobeAttachHistGetStatSimple(const UprobeAttachInterface* uprobe)
{
    HASH_SEQ_STATUS mapIterator;
    UprobeAttachHistMapEntry* mapEntry;
    UprobeAttachHistData* data = (UprobeAttachHistData*) uprobe->data;
    uint64 size = (uint64) hash_get_num_entries(data->map);
    uint64 numBins = (uint64) sqrt((double) size) + 1UL;
    uint64 min =  UINT64_MAX;
    uint64 max = 0;
    double maxUs;
    double minUs;

    if (size == 0)
        return UprobeAttachHistGetStat(uprobe, 0.0, 100.0, 10.0); // return dummy hist table

    hash_seq_init(&mapIterator, data->map);
    mapEntry = (UprobeAttachHistMapEntry*) hash_seq_search(&mapIterator);
    while (mapEntry)
    {
        if (mapEntry->time < min)
            min = mapEntry->time;

        if (mapEntry->time > max)
            max = mapEntry->time;

        mapEntry = (UprobeAttachHistMapEntry*) hash_seq_search(&mapIterator);
    }

    // put all values in the histogram range
    max += 10;
    min -= 10;

    maxUs = (double) max / 10.0;
    minUs = (double) min / 10.0;

    return UprobeAttachHistGetStat(uprobe, minUs, maxUs, (maxUs - minUs) / (double) numBins);
}



static void
UprobeStorageHistPutData(UprobeStorageHist* storage, void* data)
{
    MessageHist* mes = data;
    UprobeAttachHistMapEntry* mapEntry;
    bool isFound;
    mapEntry = (UprobeAttachHistMapEntry*) hash_search(storage->map, &mes->time, HASH_ENTER_NULL, &isFound);
    if (likely(mapEntry))
    {
        if (isFound)
            mapEntry->count += mes->count;
        else
            mapEntry->count = mes->count;
    }

}


static void
UprobeStorageHistWriteStat(UprobeStorageHist* storage, bool shouldClearStat)
{
    FILE* file;
    char filePath[MAXPGPATH];
    HASH_SEQ_STATUS mapIterator;
    UprobeAttachHistMapEntry* mapItem;
    sprintf(filePath,"%sHIST_%s.txt", dataDir, storage->base.symbol);
    file = fopen(filePath, "w");
    if (file == NULL)
    {
        elog(LOG, "can't open file %s for writting", filePath);
        return;
    }

    hash_seq_init(&mapIterator, storage->map);
    mapItem = (UprobeAttachHistMapEntry*) hash_seq_search(&mapIterator);
    fprintf(file, "time,count\n");
    while (mapItem)
    {
        double timeUs = (double) mapItem->time / 10.0;

        fprintf(file, "%.1lf,%lu\n", timeUs, mapItem->count);

        mapItem = (UprobeAttachHistMapEntry*) hash_seq_search(&mapIterator);
    }

    fclose(file);

    if (shouldClearStat)
    {
        hash_destroy(storage->map);

        storage->map = CreateHashMapForHistUprobe();
    }
}


static void
UprobeStorageHistDelete(UprobeStorageHist* storage, bool shouldWriteStat)
{
    if (shouldWriteStat)
        UprobeStorageHistWriteStat(storage, false);

    pfree(storage->base.symbol);
    hash_destroy(storage->map);
    pfree(storage);
}


UprobeStorage*
UprobeStorageHistInit(const char* symbol)
{
    UprobeStorageHist* storage = palloc(sizeof(UprobeStorageHist));
    storage->base.delete = (StorageDeleteFunc) UprobeStorageHistDelete;
    storage->base.putData = (StoragePutDataFunc) UprobeStorageHistPutData;
    storage->base.writeStat = (StorageWriteStat) UprobeStorageHistWriteStat;
    storage->base.symbol = pstrdup(symbol);
    storage->map = CreateHashMapForHistUprobe();

    return (UprobeStorage*) storage;
}


static HTAB*
CreateHashMapForMemUprobe()
{
    HTAB* map;
    HASHCTL map_info;
    map_info.keysize = sizeof(uint64);
    map_info.entrysize = sizeof(UprobeAttachMemMapEntry);
    map_info.hash = &UprobeMapKeyHash;
    map_info.match = &UprobeMapKeyCmp;
    map = hash_create("map for Mem uprobe", 1024, &map_info, HASH_ELEM | HASH_FUNCTION | HASH_COMPARE);
    return map;
}


UprobeAttachInterface*
UprobeAttachMemInit(const char* symbol)
{
    UprobeAttachInterface* res = palloc(sizeof(UprobeAttachInterface));
    UprobeAttachMemData* data = palloc(sizeof(UprobeAttachMemData));
    data->map = CreateHashMapForMemUprobe();
    res->inFunc = UprobeAttachMemInFunc;
    res->retFunc = UprobeAttachMemRetFunc;
    res->timedCallback = UprobeAttachMemTimedCallback;
    res->cleanFunc = UprobeAttachMemClean;
    res->needRetVal = false;
    res->numArgs = 0;
    res->data = data;
    res->targetSymbol = pstrdup(symbol);
    return res;
}


static void
UprobeAttachMemClean(UprobeAttachInterface* this)
{
    UprobeAttachMemData* data;
    if (this == NULL)
        return;
    data = (UprobeAttachMemData*) this->data;
    hash_destroy(data->map);
    pfree(data);
    pfree(this->targetSymbol);
    pfree(this);
}


static void
UprobeAttachMemInFunc(UprobeAttachMemData* data)
{
    data->prevMemCount = (int64) MemoryContextMemAllocated(TopMemoryContext, true);
}


static void
UprobeAttachMemRetFunc(UprobeAttachMemData* data)
{
    bool is_found;
    UprobeAttachMemMapEntry* mapEntry;
    int64 memDiff = MemoryContextMemAllocated(TopMemoryContext, true) - data->prevMemCount;
    mapEntry = (UprobeAttachMemMapEntry*) hash_search(data->map, &memDiff, HASH_ENTER_NULL, &is_found);
    if (likely(mapEntry))
    {
        if (is_found)
            mapEntry->count += 1;
        else
            mapEntry->count = 1;
    }
}


static void
UprobeAttachMemTimedCallback(UprobeAttachInterface* this)
{
    UprobeAttachMemData* data = ( UprobeAttachMemData*) this->data;
    MessageMem messages[1024];
    uint32 currentIndex = 0;
    uint32 numSend = 0;
    HASH_SEQ_STATUS mapIterator;
    UprobeAttachMemMapEntry* mapEntry;
    hash_seq_init(&mapIterator, data->map);
    mapEntry = (UprobeAttachMemMapEntry*) hash_seq_search(&mapIterator);
    while (mapEntry)
    {
        messages[currentIndex].base.type = MESSAGE_CUSTOM;
        messages[currentIndex].base.size = sizeof(MessageMem);
        messages[currentIndex].memory = mapEntry->memory;
        messages[currentIndex].count = mapEntry->count;
        currentIndex++;
        if (currentIndex == 1024)
        {
            numSend = MessageBufferPut((Message*) messages, currentIndex, this->targetSymbol);
            if (numSend != currentIndex)
            {
                memmove(messages, &messages[numSend], (currentIndex - numSend) * sizeof(MessageMem));
            }
            currentIndex = currentIndex - numSend;
        }
        mapEntry = (UprobeAttachMemMapEntry*) hash_seq_search(&mapIterator);
    }
    while (currentIndex != 0)
    {
        numSend = MessageBufferPut((Message*) messages, currentIndex, this->targetSymbol);
        if (numSend != currentIndex)
        {
            memmove(messages, &messages[numSend], (currentIndex - numSend) * sizeof(MessageMem));
        }
        currentIndex = currentIndex - numSend;
    }

    hash_destroy(data->map);
    data->map = CreateHashMapForMemUprobe();
}


static void
UprobeStorageMemPutData(UprobeStorageMem* storage, void* data)
{
    MessageMem* mes = data;
    UprobeAttachMemMapEntry* mapEntry;
    bool isFound;
    mapEntry = (UprobeAttachMemMapEntry*) hash_search(storage->map, &mes->memory, HASH_ENTER_NULL, &isFound);
    if (likely(mapEntry))
    {
        if (isFound)
            mapEntry->count += mes->count;
        else
            mapEntry->count = mes->count;
    }

}


static void
UprobeStorageMemWriteStat(UprobeStorageMem* storage, bool shouldClearStat)
{
    FILE* file;
    char filePath[MAXPGPATH];
    HASH_SEQ_STATUS mapIterator;
    UprobeAttachMemMapEntry* mapItem;
    sprintf(filePath,"%sMEM_%s.txt", dataDir, storage->base.symbol);
    file = fopen(filePath, "w");
    if (file == NULL)
    {
        elog(LOG, "can't open file %s for writting", filePath);
        return;
    }

    hash_seq_init(&mapIterator, storage->map);
    mapItem = (UprobeAttachMemMapEntry*) hash_seq_search(&mapIterator);
    fprintf(file, "memory,count\n");
    while (mapItem)
    {
        fprintf(file, "%ld,%lu\n", mapItem->memory, mapItem->count);

        mapItem = (UprobeAttachMemMapEntry*) hash_seq_search(&mapIterator);
    }

    fclose(file);

    if (shouldClearStat)
    {
        hash_destroy(storage->map);

        storage->map = CreateHashMapForMemUprobe();
    }
}


static void
UprobeStorageMemDelete(UprobeStorageMem* storage, bool shouldWriteStat)
{
    if (shouldWriteStat)
        UprobeStorageMemWriteStat(storage, false);

    pfree(storage->base.symbol);
    hash_destroy(storage->map);
    pfree(storage);
}


UprobeStorage*
UprobeStorageMemInit(const char* symbol)
{
    UprobeStorageHist* storage = palloc(sizeof(UprobeStorageHist));
    storage->base.delete = (StorageDeleteFunc) UprobeStorageMemDelete;
    storage->base.putData = (StoragePutDataFunc) UprobeStorageMemPutData;
    storage->base.writeStat = (StorageWriteStat) UprobeStorageMemWriteStat;
    storage->base.symbol = pstrdup(symbol);
    storage->map = CreateHashMapForMemUprobe();

    return (UprobeStorage*) storage;
}