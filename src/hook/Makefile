MODULE_big   = chdb_hook
OBJS 		 = ../native.o ../native_writer.o ../helper.o ../helper_setup.o ../channel.o ../spawn.o $(subst .c,.o, $(wildcard *.c))

# Define CHDB_NO_FILE_SCHEME to disable the chdb_hook file:// scheme.
ifneq ($(NO_FILE_SCHEME),)
PG_CPPFLAGS += -DCHDB_NO_FILE_SCHEME=1
endif

include ../module.mk

# PGXS tracks no header dependencies; the vendored ones are module.mk's.
$(OBJS): $(wildcard *.h)
