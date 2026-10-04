# The PGXS preamble shared by the modules built in a sub-directory of src/.
# The includer sets MODULE_big and OBJS, appends its own PG_CPPFLAGS, and
# includes this file last: pgxs.mk fixes CPPFLAGS and CFLAGS as it is read.
#
# Plain assignments, so the values the top-level make passes on the command
# line keep winning.
PGCH_DIR     = ../../vendor/pg-clickhouse-c
CH_C_DIR     = $(PGCH_DIR)/clickhouse-c
PG_CFLAGS    = -Wno-declaration-after-statement -Wall -Werror
PG_CONFIG   ?= pg_config

# A module linking ../native.o must compile with the flags native.o was built
# with, and CHC_ERR_MSG_LEN must match across every module that shares it.
# -isystem keeps the vendored headers' warnings out of the -Werror build.
# PGCH_MSG_PREFIX prefixes messages pg-clickhouse-c raises like our own.
# clickhouse-c recopies reader errors through chc_err.msg, 256 bytes by
# default, so match chdb_capture_error's buffer. native.c asserts the pairing.
# Prepended, so the includer's own flags follow them as before.
ifneq ($(filter ../native.o,$(OBJS)),)
PG_CPPFLAGS := $(strip -isystem $(CH_C_DIR) -isystem $(PGCH_DIR) \
               -DPGCH_MSG_PREFIX='"chdb: "' -DCHC_ERR_MSG_LEN=4096 $(PG_CPPFLAGS))
VENDOR_HDRS  = $(wildcard $(PGCH_DIR)/*.h $(CH_C_DIR)/*.h)
endif

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# PGXS tracks no header dependencies, and the vendored libraries are all header.
$(OBJS): $(wildcard *.h ../*.h) $(VENDOR_HDRS)
