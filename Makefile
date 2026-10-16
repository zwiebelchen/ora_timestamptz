# ora_timestamptz - Oracle-compatible TIMESTAMP WITH TIME ZONE for PostgreSQL
#
# Build against an installed PostgreSQL (PGXS):
#   make PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config
#   sudo make PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config install
#   make PG_CONFIG=... installcheck

MODULE_big = ora_timestamptz
OBJS = ora_timestamptz.o ora_tstz_format.o ora_tstz_ddl.o

EXTENSION = ora_timestamptz
DATA = ora_timestamptz--1.0.sql
DOCS = README.md README.de.md
PGFILEDESC = "ora_timestamptz - Oracle-compatible TIMESTAMP WITH TIME ZONE"

REGRESS = ora_timestamptz ddl_replace

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
