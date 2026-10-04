# Flags shared by the chdb extension and the modules under src/ that link its
# objects, so that native.o and channel.o are the same objects wherever they
# are linked: the vendored headers, the warnings and the defines, then PGXS.
# The includer sets MODULE_big and OBJS first, as PGXS wants, and whatever it
# added to PG_CPPFLAGS before the include stays.
TOP      := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))..)
# Header-only dependencies, vendored as submodules. clickhouse-c comes from
# pg-clickhouse-c's own pin, its signatures naming clickhouse-c types, so a
# second checkout on the include path would silently win.
PGCH_DIR  = $(TOP)/vendor/pg-clickhouse-c
CH_C_DIR  = $(PGCH_DIR)/clickhouse-c
# The prefix on the messages pg-clickhouse-c raises, like our own.
PGCH_MSG_PREFIX ?= chdb

# Suppress the pre-C99 warning, error on the others.
PG_CFLAGS   += -Wno-declaration-after-statement -Wall -Werror
# -isystem keeps the vendored headers' warnings out of the -Werror build.
# clickhouse-c copies what it raises through chc_err.msg, 256 bytes by default,
# which clips the longer type names out of a decoding error: native.h asserts
# the size matches the channel's error buffer, in every module that links it.
PG_CPPFLAGS += -isystem $(CH_C_DIR) -isystem $(PGCH_DIR) \
               -DPGCH_MSG_PREFIX='"$(PGCH_MSG_PREFIX): "' -DCHC_ERR_MSG_LEN=4096
PG_CONFIG   ?= pg_config

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# PGXS tracks no header dependencies, and the vendored libraries are all header.
# *.bc compiles the same sources, so needs the same headers.
$(OBJS) $(OBJS:.o=.bc): $(wildcard $(PGCH_DIR)/*.h $(CH_C_DIR)/*.h)

# Run make print-VARIABLE_NAME to print VARIABLE_NAME's flavor and value.
print-%: ; $(info $* is $(flavor $*) variable set to "$($*)") @true
