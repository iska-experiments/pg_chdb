MODULE_big   = chdb_search
OBJS         = ../native.o ../channel.o $(subst .c,.o, $(wildcard *.c))
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
