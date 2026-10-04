PROGRAM  = chdb_search_engine
OBJS     = $(subst .c,.o, $(wildcard *.c))
# override, so a CFLAGS on the command line adds to the project flags.
override CFLAGS  += -Wall -Werror -O2

TOP = ../../..
include ../../libchdb.mk

all: $(PROGRAM)

$(PROGRAM): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJS): $(wildcard *.h) ../protocol.h ../../setup.h

.PHONY: clean
clean:
	rm -f $(PROGRAM) $(OBJS)
