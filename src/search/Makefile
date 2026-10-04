MODULE_big   = chdb_search
OBJS         = $(subst .c,.o, $(wildcard *.c))

# engine.c only needs chdb.h for types; it reaches libchdb through dlsym.
ifneq ($(LIBCHDB_DIR),)
PG_CPPFLAGS += -isystem ../../$(LIBCHDB_DIR)/include
endif

include ../module.mk
