#include "postgres.h"

#include "list.h"

void
ListInit(UprobeList** list, CompareFunction comparator, MemoryContext memoryContext) 
{
    MemoryContext old = MemoryContextSwitchTo(memoryContext);                         
    *list = (UprobeList*)palloc0(sizeof(UprobeList));
    MemoryContextSwitchTo(old);
    (*list)->comparator = comparator;
    (*list)->memoryContext = memoryContext;
    return;
}

void
ListAdd(UprobeList* list, void* value) {  
    MemoryContext old = MemoryContextSwitchTo(list->memoryContext);                         
    ListItem* item = (ListItem*)palloc0(sizeof(ListItem));
    MemoryContextSwitchTo(old);
    item->value = value;

    if (list->head == NULL) 
    {
        list->head = item;
        list->tail = list->head;
        ++list->ListSize;
        return;
    }

    item->prev = list->tail;
    list->tail->next = item;
    list->tail = item;
    ++list->ListSize;
    return;
}

void*
ListPop(UprobeList* list, void* value) 
{
    ListItem* current;
    ListItem* prev;

    if (list->head == NULL) 
        return NULL;
    if(!list->comparator(list->head->value, value))
    {
        ListItem* next = list->head->next;
        void* res = list->head->value;
        pfree(list->head);
        --list->ListSize;
        list->head = next;
        if (next)
            next->prev = NULL;
        else
            list->tail = NULL;
        return res;
    }
    
    current = list->head->next;
    prev = list->head;

    while(current != NULL) 
    {
        if (!list->comparator(current->value, value)) 
        {
            ListItem* temp = current->next;
            void* res = current->value;
            pfree(current);
            current = temp;
            prev->next = current;
            --list->ListSize;
            if (current == NULL)
                list->tail = prev;
            else
                current->prev = prev;
            return res;
        }
        prev = current;
        current = current->next;
    }
    return NULL;
}


void*
ListPopLast(UprobeList* list)
{
    void* res;
    if (list == NULL || list->tail == NULL)
        return NULL;

    res = list->tail->value;
    --list->ListSize;
    if (list->ListSize == 0)
    {
        pfree(list->tail);
        list->head = NULL;
        list->tail = NULL;
    }
    else
    {
        ListItem* saveTail = list->tail;
        list->tail->prev->next = NULL;
        list->tail = list->tail->prev;
        pfree(saveTail);
    }
    return res;
}


void*
ListPopFirst(UprobeList* list)
{
    void* res;
    if (list == NULL || list->head == NULL)
        return NULL;

    res = list->head->value;
    --list->ListSize;
    if (list->ListSize == 0)
    {
        pfree(list->head);
        list->head = NULL;
        list->tail = NULL;
    }
    else
    {
        ListItem* saveHead = list->head;
        list->head->next->prev = NULL;
        list->head = list->head->next;
        pfree(saveHead);
    }
    return res;
}


size_t
ListSize(UprobeList* list) 
{
    return list->ListSize;
}


bool
ListContains(UprobeList* list, void* value) 
{
    for(ListItem* item = list->head; item != NULL; item = item->next) 
    {
        if (!list->comparator(item->value, value)) 
            return true;
    }
    return false;
}


void
ListFree(UprobeList* list) 
{
    ListItem* list_iterator = list->head;
    
    while(list_iterator != NULL) 
    {
        ListItem* next = list_iterator->next;
        pfree(list_iterator);
        list_iterator = next;
    }
    pfree(list);
}


void
ListMakeEmpty(UprobeList* list)
{
    ListItem* list_iterator = list->head;

    while(list_iterator != NULL)
    {
        ListItem* next = list_iterator->next;
        pfree(list_iterator);
        list_iterator = next;
    }
    list->ListSize = 0;
    list->head = NULL;
    list->tail = NULL;

}
