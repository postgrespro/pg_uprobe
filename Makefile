# contrib/pg_uprobe/Makefile

# cmake install frida
RUN_CMAKE_TO_INSTALL_FRIDA_0 := $(shell mkdir -p build )
RUN_CMAKE_TO_INSTALL_FRIDA_1 := $(shell cmake -B ./build -S .)
RUN_CMAKE_TO_INSTALL_FRIDA_2 := $(shell cmake --build ./build)
PG_CFLAGS += -I./build/FridaGum-prefix/src/FridaGum
PG_CPPFLAGS += -I./build/FridaGum-prefix/src/FridaGum
SHLIB_LINK += ./build/FridaGum-prefix/src/FridaGum/libfrida-gum.a

MODULE_big = pg_uprobe
OBJS = \
	$(WIN32RES) \
	src/pg_uprobe.o \
	src/uprobe_internal.o \
	src/list.o \
	src/uprobe_collector.o \
	src/uprobe_message_buffer.o \
	src/uprobe_shared_config.o \
	src/count_uprobes.o \
	src/uprobe_factory.o \
	src/trace_execute_nodes.o \
	src/trace_lock_on_buffers.o \
	src/trace_parsing.o \
	src/trace_planning.o \
	src/trace_session.o \
	src/trace_wait_events.o \
	src/json_to_jsonbvalue_parser.o \
	src/lockmanager_trace.o \
	src/trace_file.o

PG_CFLAGS += -I./src/include
PG_CPPFLAGS += -I./src/include
PGFILEDESC = "pg_uprobe - allows measuring postgres functions execution time"

EXTENSION = pg_uprobe
DATA = pg_uprobe--1.0.sql

REGRESS = pg_uprobe

SHLIB_LINK += $(filter -lm, $(LIBS))
EXTRA_CLEAN = node_names.h

ifdef USE_PGXS
PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
PG_INCLUDE_DIR = $(shell $(PG_CONFIG) --includedir-server)
include $(PGXS)
else
subdir = contrib/pg_uprobe
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
PG_INCLUDE_DIR = ../../src/include
endif
GEN_LOG := $(shell python3 gen_node_names_array.py $(MAJORVERSION) $(PG_INCLUDE_DIR)/nodes node_names.h)


python_tests:
	python3 ./tests/main.py