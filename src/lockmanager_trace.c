#include "postgres.h"
#include <time.h>
#include "utils/hsearch.h"
#include "storage/lwlock.h"

#include "uprobe_message_buffer.h"
#include "trace_file.h"

#include "lockmanager_trace.h"

#define INVALID_PARTITION -1

typedef struct LockManagerMapKey
{
    int partition;
    uint64 time;
} LockManagerMapKey;


typedef struct LockManagerMapEntry
{
    LockManagerMapKey key;
    uint64 count;
} LockManagerMapEntry;


typedef struct LockManagerStorage
{
    UprobeStorage base;

    HTAB* map;
} LockManagerStorage;


typedef struct LockManagerMessage
{
    Message base;

    LockManagerMapEntry entry;
} LockManagerMessage;


typedef struct LockManagerData
{
    HTAB* map;
} LockManagerData;


//lock Acquire data
static int partition;
static uint64 acquireTime;


static void LWLockAcquireInFunc(void* data, LWLock* lock);
static void LWLockAcquireRetFunc(void* data);
static void LWLockAcquireClean(UprobeAttachInterface* this);
static HTAB* CreateLockManagerHashMap(void);
static void LWLockReleaseInFunc(LockManagerData* data);
static void LWLockReleaseRetFunc(LockManagerData* data);
static void LWLockReleaseTimedCallback(UprobeAttachInterface* this);
static void LWLockReleaseClean(UprobeAttachInterface* this);
static void LockManagerStoragePutData(LockManagerStorage* storage, LockManagerMessage* mes);
static void LockManagerStorageWriteStat(LockManagerStorage* storage, bool shouldClearStat);
static void LockManagerStorageDelete(LockManagerStorage* storage, bool shouldWriteStat);


static void
LWLockAcquireInFunc(void* data, LWLock* lock)
{
    LWLockPadded* l;
    partition = INVALID_PARTITION;
    if (lock->tranche != LWTRANCHE_LOCK_MANAGER)
        return;

    l = MainLWLockArray + LOCK_MANAGER_LWLOCK_OFFSET;
    for (int id = 0; id < NUM_LOCK_PARTITIONS; id++, l++)
    {
        if (&l->lock == lock)
            partition = id;
    }
}


static void
LWLockAcquireRetFunc(void* data)
{
    struct timespec time;
    if (partition == INVALID_PARTITION)
        return;


    clock_gettime(CLOCK_MONOTONIC, &time);
    acquireTime = time.tv_nsec + time.tv_sec * 1000000000L;
}


static void
LWLockAcquireClean(UprobeAttachInterface* this)
{
    if (this == NULL)
        return;

    pfree(this->targetSymbol);
    pfree(this);
}


static HTAB*
CreateLockManagerHashMap(void)
{
    HTAB* map;
    HASHCTL map_info;
    map_info.keysize = sizeof(LockManagerMapKey);
    map_info.entrysize = sizeof(LockManagerMapEntry);
    map = hash_create("map for Lock Manager trace", 1024, &map_info, HASH_ELEM | HASH_BLOBS);
    return map;
}


static void
LWLockReleaseInFunc(LockManagerData* data)
{
    struct timespec time;
    uint64 timeDiff;
    LockManagerMapKey key;
    LockManagerMapEntry* entry;
    bool isFound;
    if (partition == INVALID_PARTITION)
        return;

    clock_gettime(CLOCK_MONOTONIC, &time);
    timeDiff = time.tv_nsec + time.tv_sec * 1000000000L - acquireTime;
    key.partition = partition;
    key.time = timeDiff / 100 + ((timeDiff % 100 >= 50) ? 1: 0);

    entry = (LockManagerMapEntry*) hash_search(data->map, &key, HASH_ENTER_NULL, &isFound);
    if (likely(entry))
    {
        if (isFound)
            entry->count++;
        else
            entry->count = 1;
    }
}


static void
LWLockReleaseRetFunc(LockManagerData* data)
{

}


static void
LWLockReleaseTimedCallback(UprobeAttachInterface* this)
{
    LockManagerData* data = ( LockManagerData*) this->data;
    LockManagerMessage messages[1024];
    uint32 currentIndex = 0;
    uint32 numSend = 0;
    HASH_SEQ_STATUS mapIterator;
    LockManagerMapEntry* mapEntry;
    hash_seq_init(&mapIterator, data->map);
    mapEntry = (LockManagerMapEntry*) hash_seq_search(&mapIterator);
    while (mapEntry)
    {
        messages[currentIndex].base.type = MESSAGE_CUSTOM;
        messages[currentIndex].base.size = sizeof(LockManagerMessage);
        messages[currentIndex].entry = *mapEntry;
        currentIndex++;
        if (currentIndex == 1024)
        {
            numSend = MessageBufferPut((Message*) messages, currentIndex, this->targetSymbol);
            if (numSend != currentIndex)
            {
                memmove(messages, &messages[numSend], (currentIndex - numSend) * sizeof(LockManagerMessage));
            }
            currentIndex = currentIndex - numSend;
        }
        mapEntry = (LockManagerMapEntry*) hash_seq_search(&mapIterator);
    }
    while (currentIndex != 0)
    {
        numSend = MessageBufferPut((Message*) messages, currentIndex, this->targetSymbol);
        if (numSend != currentIndex)
        {
            memmove(messages, &messages[numSend], (currentIndex - numSend) * sizeof(LockManagerMessage));
        }
        currentIndex = currentIndex - numSend;
    }

    hash_destroy(data->map);
    data->map = CreateLockManagerHashMap();
}


static void
LWLockReleaseClean(UprobeAttachInterface* this)
{
    LockManagerData* data;
    if (!this)
        return;
    data = (LockManagerData*) this->data;
    hash_destroy(data->map);
    pfree(data);
    pfree(this->targetSymbol);
    pfree(this);
}


static void
LockManagerStoragePutData(LockManagerStorage* storage, LockManagerMessage* mes)
{
    LockManagerMapEntry* entry;
    bool isFound;

    entry = (LockManagerMapEntry*) hash_search(storage->map, &mes->entry.key, HASH_ENTER_NULL, &isFound);
    if (likely(entry))
    {
        if (isFound)
        {
            entry->count += mes->entry.count;
        }
        else
        {
            entry->count = mes->entry.count;
        }
    }
}


static void
LockManagerStorageWriteStat(LockManagerStorage* storage, bool shouldClearStat)
{
    FILE* files[256];
    char filePath[MAXPGPATH];
    HASH_SEQ_STATUS mapIterator;
    LockManagerMapEntry* mapEntry;

    for (int i = 0; i < NUM_LOCK_PARTITIONS; i++)
    {
        sprintf(filePath,"%sLock_Manager_%d.txt", dataDir, i);
        files[i] = fopen(filePath, "w");
        if (files[i] == NULL)
        {
            elog(LOG, "can't open file %s for writting", filePath);

            for (int j = 0; j < i; j++)
                fclose(files[j]);

            return;
        }
        fprintf(files[i], "time,count\n");
    }

    hash_seq_init(&mapIterator, storage->map);
    mapEntry = (LockManagerMapEntry*) hash_seq_search(&mapIterator);
    while (mapEntry)
    {
        double time = (double) mapEntry->key.time / 10.0;
        fprintf(files[mapEntry->key.partition], "%.1lf,%lu\n", time, mapEntry->count);

        mapEntry = (LockManagerMapEntry*) hash_seq_search(&mapIterator);
    }

    for (int i = 0; i < NUM_LOCK_PARTITIONS; i++)
        fclose(files[i]);

    if (shouldClearStat)
    {
        hash_destroy(storage->map);

        storage->map = CreateLockManagerHashMap();
    }
}


static void
LockManagerStorageDelete(LockManagerStorage* storage, bool shouldWriteStat)
{
    if (shouldWriteStat)
        LockManagerStorageWriteStat(storage, false);
    hash_destroy(storage->map);
    pfree(storage->base.symbol);
    pfree(storage);
}


UprobeAttachInterface*
LWLockAcquireInit(const char* symbol)
{
    UprobeAttachInterface* res = palloc(sizeof(UprobeAttachInterface));
    res->inFunc = LWLockAcquireInFunc;
    res->retFunc = LWLockAcquireRetFunc;
    res->timedCallback = NULL;
    res->cleanFunc = LWLockAcquireClean;
    res->needRetVal = false;
    res->numArgs = 1;
    res->data = NULL;
    res->targetSymbol = pstrdup(symbol);
    return res;
}

UprobeAttachInterface*
LWLockReleaseInit(const char* symbol)
{
    UprobeAttachInterface* res = palloc(sizeof(UprobeAttachInterface));
    LockManagerData* storage = palloc(sizeof(LockManagerData));
    res->inFunc = LWLockReleaseInFunc;
    res->retFunc = LWLockReleaseRetFunc;
    res->timedCallback = LWLockReleaseTimedCallback;
    res->cleanFunc = LWLockReleaseClean;
    res->needRetVal = false;
    res->numArgs = 0;
    res->data = storage;
    res->targetSymbol = pstrdup(symbol);
    storage->map = CreateLockManagerHashMap();
    return res;
}

UprobeStorage*
LockManagerStorageInit(const char* symbol)
{
    LockManagerStorage* storage = palloc(sizeof(LockManagerStorage));
    storage->base.delete = (StorageDeleteFunc) LockManagerStorageDelete;
    storage->base.putData = (StoragePutDataFunc) LockManagerStoragePutData;
    storage->base.writeStat = (StorageWriteStat) LockManagerStorageWriteStat;
    storage->base.symbol = pstrdup(symbol);
    storage->map = CreateLockManagerHashMap();

    return (UprobeStorage*) storage;
}

UprobeStorage*
NullStorageInit(const char* symbol)
{
    return NULL;
}