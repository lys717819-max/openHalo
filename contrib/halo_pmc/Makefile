MODULE_big = halo_pmc_ext
OBJS = halo_pmc_ext.o

EXTENSION = halo_pmc
DATA = halo_pmc--1.0.sql

PGFILEDESC = "halo_pmc - CPU performance monitoring"

ifdef USE_PGXS
PG_CONFIG = /opt/openhalo/bin/pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/halo_pmc
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif