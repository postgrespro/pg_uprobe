#ifndef UPROBE_SHARED_CONFIG_H
#define UPROBE_SHARED_CONFIG_H


typedef void (*LoadFromConfigApplyFunc)(const char* func, const char* type);


extern void PGUprobeSaveInSharedConfig(char* func, char* type);


extern void PGUprobeLoadFromSharedConfig(LoadFromConfigApplyFunc applyFunc);


extern void PGUprobeDeleteFromSharedConfig(const char* func);

#endif