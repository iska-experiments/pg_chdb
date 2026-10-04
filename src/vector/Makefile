MODULE_big   = chdb_vector
OBJS         = $(subst .c,.o, $(wildcard *.c))

# The flags the other modules were built with, and PGXS.
include ../module.mk

# PGXS tracks no header dependencies; the vendored ones are module.mk's.
$(OBJS): $(wildcard *.h ../*.h)
