MODULE_big   = chdb_hook
# Every native_*.c beside native.c: the Native stream, both directions.
NATIVE_OBJS  = $(patsubst %.c,%.o,$(wildcard ../native*.c))
OBJS 		 = $(NATIVE_OBJS) ../helper.o ../helper_setup.o ../channel.o ../spawn.o $(subst .c,.o, $(wildcard *.c))

# Define CHDB_NO_FILE_SCHEME to disable the chdb_hook file:// scheme.
ifneq ($(NO_FILE_SCHEME),)
PG_CPPFLAGS += -DCHDB_NO_FILE_SCHEME=1
endif

include ../module.mk

# PGXS tracks no header dependencies; the vendored ones are module.mk's.
$(OBJS): $(wildcard *.h)
