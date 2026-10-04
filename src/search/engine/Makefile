PROGRAM  = chdb_search_engine
OBJS     = $(subst .c,.o, $(wildcard *.c))
TOP      = ../../..
# dlsym, for the check that the library loaded has the callback object
# storage; the reader of the supervisor's replies is a thread.
LDLIBS   = -ldl -lpthread
include ../../libchdb.mk

$(OBJS): $(wildcard *.h) ../protocol.h ../pagestore/protocol.h ../../setup.h
