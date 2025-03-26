#ifndef UPROBE_INTERNAL_H
#define UPROBE_INTERNAL_H


#include "postgres.h"

#include "uprobe_attach_interface.h"

typedef enum 
{
    SUCCESS, INTERNAL_ERROR, CANNOT_FIND_SYMBOL, INVALID_NUMBER_OF_ARGS
} UPROBE_INIT_RES;

typedef struct Uprobe Uprobe;


extern MemoryContext UprobeMemoryContext;


extern void UprobeInternalInit(void);

extern void UprobeInternalFini(void);

extern UPROBE_INIT_RES UprobeInit(UprobeAttachInterface* uprobeAttach, Uprobe** uprobe);

extern void UprobeDelete(Uprobe* uprobe);

extern int UprobeCompare(Uprobe* uprobe, char* func);

extern const char* UprobeGetFunc(Uprobe* uprobe);

extern void UprobeCallTimedCallback(Uprobe* uprobe);

extern const UprobeAttachInterface* UprobeGetAttachInterface(Uprobe* uprobe);

#endif            /*UPROBE_INTERNAL_H*/