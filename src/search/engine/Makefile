PROGRAM  = chdb_search_engine
OBJS     = $(subst .c,.o, $(wildcard *.c))
TOP      = ../../..
include ../../libchdb.mk

$(OBJS): $(wildcard *.h) ../protocol.h ../../setup.h
