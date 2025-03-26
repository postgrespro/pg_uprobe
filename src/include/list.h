#ifndef LIST_H
#define LIST_H
#include "postgres.h"

typedef int (*CompareFunction)(const void*, const void*);

typedef struct ListItem ListItem;

struct ListItem {
    void* value;
    ListItem* next;
    ListItem* prev;
};

typedef struct 
{
    ListItem* head;
    ListItem* tail;
    size_t ListSize;

    MemoryContext memoryContext;
    CompareFunction comparator;
} UprobeList;

extern void ListInit(UprobeList** list, CompareFunction comparator, MemoryContext memoryContext);
extern void ListAdd(UprobeList* list, void* value);
extern void* ListPop(UprobeList* list, void* value);

extern void* ListPopLast(UprobeList* list);

extern void* ListPopFirst(UprobeList* list);

extern size_t ListSize(UprobeList* list);
extern bool ListContains(UprobeList* list, void* value);

extern void ListMakeEmpty(UprobeList* list);
extern void ListFree(UprobeList* list);

#define LIST_FOREACH(list, iterator)                                                                    \
    for(ListItem* iterator = (list)->head; ((void *) iterator) != NULL; (iterator) = (iterator)->next)  \

#define LIST_FOREACH_REVERSE(list, iterator)                                                            \
    for(ListItem* iterator = (list)->tail; ((void *) iterator) != NULL; (iterator) = (iterator)->prev)  \

#define LIST_LAST(list) list ? list->tail->value : NULL
#endif