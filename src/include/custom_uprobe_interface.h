#ifndef CUSTOM_UPROBE_INTERFACE_H
#define CUSTOM_UPROBE_INTERFACE_H
#include "uprobe_attach_interface.h"

struct UprobeStorage;

typedef void (*StorageDeleteFunc)(struct UprobeStorage* storage, bool shouldWriteStat);

typedef void (*StoragePutDataFunc)(struct UprobeStorage* storage, void* data);

typedef void (*StorageWriteStat)(struct UprobeStorage* storage, bool shouldClearStat);

//abstract struct for all storages
typedef struct UprobeStorage
{
    StoragePutDataFunc putData;
    StorageWriteStat writeStat;
    StorageDeleteFunc delete;
    char* symbol;
} UprobeStorage;


#endif