# Build with the pg_config of whichever PostgreSQL installation you want
# pg_vm to match (BLCKSZ, page layout version, etc. all come from its
# headers), e.g.:
#   make -f Makefile.pg_vm PG_CONFIG=~/pginstall/bin/pg_config
PROGRAM = pg_fsm
OBJS = pg_fsm.o

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

LIBS = -L$(pkglibdir) -lpgcommon -lpgport