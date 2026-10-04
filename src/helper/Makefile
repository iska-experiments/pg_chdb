PROGRAM  = chdb_helper
OBJS     = $(PROGRAM).o
# override, so a CFLAGS on the command line adds to the project flags.
override CFLAGS  += -Wall -Werror -O2

TOP = ../..
include ../libchdb.mk

all: $(PROGRAM)

$(OBJS): chdb_helper.c ../setup.h

.PHONY: clean
clean:
	rm -f $(PROGRAM) $(OBJS)
