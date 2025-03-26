#include "postgres.h"
#include <signal.h>
#include "utils/elog.h"
#include "utils/memutils.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"

#include "list.h"
#include "uprobe_shared_config.h"
#include "uprobe_factory.h"
#include "uprobe_message_buffer.h"


static UprobeList* storageList = NULL;

static char currentSymbol[MAX_SYMBOL_SIZE + 1] = {'\0'};

static UprobeStorage* currentStorage = NULL;

static MemoryContext collectorContext = NULL;


PGDLLEXPORT void StatCollectorMain(void);

static int StorageListComparator(UprobeStorage* entry, char* symbol);
static void ProcessMessageNewSharedUprobe(MessageNewSharedUprobe* mes);
static void ProcessMessageWriteStat(MessageWriteStat* mes);
static void ProcessMessageDeleteSharedUprobe(MessageDeleteSharedUprobe* mes);
static void ProcessMessageSymbol(MessageSymbol* mes);
static void ProcessMessageCustom(Message* mes);
static void ProcessOneMessage(Message* mes);

static void TermSignalHandler(SIGNAL_ARGS);
static void StatCollectorAtExitCallback(void);
static void StatCollectorSharedConfigApplyFunc(const char* func, const char* type);

static int
StorageListComparator(UprobeStorage* entry, char* symbol)
{
    return strcmp(entry->symbol, symbol);
}


static void
ProcessMessageNewSharedUprobe(MessageNewSharedUprobe* mes)
{
    UprobeStorage* newEntry;
    if (ListContains(storageList, (void*) currentSymbol))
        return;

    newEntry = GetUprobeStorageForType(mes->uprobeType, currentSymbol);

    if (!newEntry)
        return;

    ListAdd(storageList, newEntry);
}


static void
ProcessMessageWriteStat(MessageWriteStat* mes)
{
    if (currentStorage)
        currentStorage->writeStat(currentStorage, mes->shouldEmptyData);

    if (mes->latch)
        SetLatch(mes->latch);
}


static void
ProcessMessageDeleteSharedUprobe(MessageDeleteSharedUprobe* mes)
{
    UprobeStorage* entryToDelete = ListPop(storageList, currentSymbol);

    if (entryToDelete == NULL)
    {
        if (mes->latch && mes->shouldWriteStat)
            SetLatch(mes->latch);
        return;
    }
    entryToDelete->delete(entryToDelete, mes->shouldWriteStat);
    if (mes->latch && mes->shouldWriteStat)
        SetLatch(mes->latch);
}


static void
ProcessMessageSymbol(MessageSymbol* mes)
{
    uint16 symbolLen = mes->base.size - sizeof(MessageSymbol);
    memcpy(currentSymbol, mes->symbol, symbolLen);

    currentSymbol[symbolLen] = '\0';

    currentStorage = NULL;

    LIST_FOREACH(storageList, it)
    {
        UprobeStorage* storage = (UprobeStorage*) it->value;
        if (strcmp(storage->symbol, currentSymbol) == 0)
        {
            currentStorage = storage;
            break;
        }
    }
}


static void
ProcessMessageCustom(Message* mes)
{
    if (currentStorage)
        currentStorage->putData(currentStorage, mes);
}


static void
ProcessOneMessage(Message* mes)
{
    switch (mes->type)
    {
    case MESSAGE_NEW_SHARED_UPROBE:
    {
        ProcessMessageNewSharedUprobe((MessageNewSharedUprobe*) mes);
        break;
    }
    case MESSAGE_WRITE_STAT:
    {
        ProcessMessageWriteStat((MessageWriteStat*) mes);
        break;
    }
    case MESSAGE_DELETE_SHARED_UPROBE:
    {
        ProcessMessageDeleteSharedUprobe((MessageDeleteSharedUprobe*) mes);
        break;
    }
    case MESSAGE_SYMBOL:
    {
        ProcessMessageSymbol((MessageSymbol*) mes);
        break;
    }
    case MESSAGE_CUSTOM:
    {
        ProcessMessageCustom(mes);
        break;
    }
    default:
        elog(LOG, "invalid message type %d for symbol %128s", mes->type, currentSymbol);
    }
}


static void
TermSignalHandler(SIGNAL_ARGS)
{
    MessageBufferDelete();
    proc_exit(0);
}


static void
StatCollectorAtExitCallback(void)
{
    LIST_FOREACH(storageList, it)
    {
        UprobeStorage* storage = (UprobeStorage*) it->value;
        storage->writeStat(storage, false);
    }
}


static void
StatCollectorSharedConfigApplyFunc(const char* func, const char* type)
{
    MessageNewSharedUprobe mes;
    mes.uprobeType = GetTypeByCharName(type);

    if (mes.uprobeType == INVALID_TYPE)
    {
        elog(WARNING, "invalid attach type %128s", type);
        return;
    }

    strncpy(currentSymbol, func, 255);

    ProcessMessageNewSharedUprobe(&mes);
}


void
StatCollectorMain(void)
{
    Message* messages;
    uint32 currentNumberOfMessages;

    pqsignal(SIGTERM, TermSignalHandler);
    pqsignal(SIGHUP, SIG_IGN);
    pqsignal(SIGUSR1, SIG_IGN);
    BackgroundWorkerUnblockSignals();

    collectorContext = AllocSetContextCreate(NULL, "pg_uprobe collector", ALLOCSET_DEFAULT_SIZES);
    MemoryContextSwitchTo(collectorContext);


    ListInit(&storageList, (CompareFunction) &StorageListComparator, collectorContext);

    PGUprobeLoadFromSharedConfig(&StatCollectorSharedConfigApplyFunc);
    before_shmem_exit((pg_on_exit_callback) StatCollectorAtExitCallback, (Datum) 0);
    messages = palloc(MESSAGEBUFFER_SIZE);
    while (true)
    {
        Message* currentMessage;
        currentNumberOfMessages = MessageBufferGet(messages, MESSAGEBUFFER_SIZE);

        currentMessage = messages;
        for (uint32 i = 0; i < currentNumberOfMessages; i++)
        {
            ProcessOneMessage(currentMessage);

            currentMessage = (Message*) (((char*) currentMessage) + currentMessage->size);
        }
    }
}