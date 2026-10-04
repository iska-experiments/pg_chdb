MODULE_big   = chdb_search
# The worker client, or with CHDB_SEARCH_STUB=1 a per-backend fake answering
# from GUCs, so the access method builds and tests on its own; a tree without
# client.c links the fake without being told, and says so.
CHDB_SEARCH_STUB ?= $(if $(wildcard client.c),,1)
export CHDB_SEARCH_STUB
ifneq ($(CHDB_SEARCH_STUB),)
$(info chdb_search: linking the stub worker client)
endif
CLIENT       = $(if $(CHDB_SEARCH_STUB),client_stub.c,client.c)
# The custom scan of planner/ is part of the module: one set of GUCs, one
# library to load.
OBJS         = ../native.o ../native_writer.o ../channel.o ../helper_setup.o ../spawn.o $(subst .c,.o, $(filter-out client.c client_stub.c,$(wildcard *.c planner/*.c)) $(CLIENT))

include ../module.mk

# PGXS tracks no header dependencies; the vendored ones are module.mk's.
$(OBJS): $(wildcard *.h ../*.h planner/*.h)

# client.mode, written by the top-level Makefile, changes with CHDB_SEARCH_STUB.
$(shlib): $(wildcard client.mode)
