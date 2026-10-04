PROGRAM  = chdb_search_engine
OBJS     = $(subst .c,.o, $(wildcard *.c))
TOP      = ../../..
# dlsym, for the check that the library loaded has the callback object storage.
LDLIBS   = -ldl
include ../../libchdb.mk

$(OBJS): $(wildcard *.h) ../protocol.h ../../setup.h
