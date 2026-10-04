MODULE_big   = chdb_search
# The worker client, or with CHDB_SEARCH_STUB=1 a per-backend fake that logs
# and discards everything, so the access method builds and tests on its own.
CLIENT       = $(if $(CHDB_SEARCH_STUB),client_stub.c,client.c)
OBJS         = ../native.o ../channel.o ../helper_setup.o ../spawn.o $(subst .c,.o, $(filter-out client.c client_stub.c,$(wildcard *.c)) $(CLIENT))

include ../module.mk

# client.mode, written by the top-level Makefile, changes with CHDB_SEARCH_STUB.
$(shlib): $(wildcard client.mode)
