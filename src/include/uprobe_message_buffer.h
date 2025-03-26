#ifndef UPROBE_MESSAGE_BUFFER_H
#define UPROBE_MESSAGE_BUFFER_H

#include "postgres.h"
#include "storage/latch.h"
#include "utils/wait_event.h"

#if PG_MAJORVERSION_NUM < 17
#define END_OF_WRITE_STAT_EVENT WAIT_EVENT_MQ_INTERNAL
#else
#define END_OF_WRITE_STAT_EVENT WAIT_EVENT_MESSAGE_QUEUE_INTERNAL
#endif

#define ESTIMATE_MESSAGE_SIZE (sizeof(Message) + 255)

#define MESSAGEBUFFER_SIZE (ESTIMATE_MESSAGE_SIZE * 1024)

#define MAX_SYMBOL_SIZE 1024

#define MESSAGE_SYMBOL 0
#define MESSAGE_DELETE_SHARED_UPROBE 1
#define MESSAGE_NEW_SHARED_UPROBE 2
#define MESSAGE_WRITE_STAT 3
#define MESSAGE_CUSTOM 4


//base struct for all messages, should be first field in all messages
typedef struct Message
{
    uint16 type;
    uint16 size;
} Message;


typedef struct MessageNewSharedUprobe
{
    Message base;
    uint8 uprobeType;
} MessageNewSharedUprobe;


typedef struct MessageWriteStat
{
    Message base;
    bool shouldEmptyData;
    Latch* latch;    //if not null will be set after stat is written
} MessageWriteStat;


typedef struct MessageDeleteSharedUprobe
{
    Message base;
    bool shouldWriteStat;
    Latch* latch;    //if (not null and shouldWriteStat is true) will be set after stat is written
} MessageDeleteSharedUprobe;


typedef struct MessageSymbol
{
    Message base;
    char symbol[FLEXIBLE_ARRAY_MEMBER];
} MessageSymbol;


extern void MessageBufferRequest(void);


extern void MessageBufferInit(void);

extern void MessageBufferDelete(void);

extern int MessageBufferPut(const Message* mes, uint32 n, char* symbol);

extern int MessageBufferGet(Message* mes, uint32 bufferSize);


#endif      /*UPROBE_MESSAGE_BUFFER_H*/