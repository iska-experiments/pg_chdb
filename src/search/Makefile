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
OBJS         = ../native.o ../channel.o $(subst .c,.o, $(filter-out client.c client_stub.c,$(wildcard *.c)) $(CLIENT))
PGCH_DIR     = ../../vendor/pg-clickhouse-c
CH_C_DIR     = $(PGCH_DIR)/clickhouse-c
PG_CFLAGS    = -Wno-declaration-after-statement -Wall -Werror
# Same flags as the hook module: debug.c includes pg-clickhouse.h for the type
# mapping, and native.o was built with them.
PG_CPPFLAGS  = -isystem $(CH_C_DIR) -isystem $(PGCH_DIR) -DPGCH_MSG_PREFIX='"chdb: "' \
               -DCHC_ERR_MSG_LEN=4096
PG_CONFIG   ?= pg_config

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# PGXS tracks no header dependencies, and the vendored libraries are all header.
$(OBJS): $(wildcard *.h ../*.h $(PGCH_DIR)/*.h $(CH_C_DIR)/*.h)

# client.mode, written by the top-level Makefile, changes with CHDB_SEARCH_STUB.
$(shlib): $(wildcard client.mode)
