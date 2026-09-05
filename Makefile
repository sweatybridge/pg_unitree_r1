EXTENSION = pg_unitree_r1
MODULE_big = pg_unitree_r1
OBJS = src/extension.o src/unitree_gateway.o src/control_core.o
EXTVERSION := $(shell awk -F"'" '/^default_version[[:space:]]*=/{print $$2; exit}' '$(CURDIR)/pg_unitree_r1.control')
DATA = sql/pg_unitree_r1--$(EXTVERSION).sql

ifeq ($(strip $(EXTVERSION)),)
$(error could not read default_version from pg_unitree_r1.control)
endif

UNITREE_ROOT ?= ..
UNITREE_ARCH ?= $(shell uname -m)

PG_CPPFLAGS += -I$(srcdir)/include \
	-I$(srcdir)/$(UNITREE_ROOT)/include \
	-I$(srcdir)/$(UNITREE_ROOT)/thirdparty/include \
	-I$(srcdir)/$(UNITREE_ROOT)/thirdparty/include/ddscxx
PG_CXXFLAGS += -std=c++17 -Wall -Wextra
SHLIB_LINK += $(srcdir)/$(UNITREE_ROOT)/lib/$(UNITREE_ARCH)/libunitree_sdk2.a \
	-L$(srcdir)/$(UNITREE_ROOT)/thirdparty/lib/$(UNITREE_ARCH) \
	-Wl,-rpath,'$$ORIGIN' -lddscxx -lddsc -lstdc++ -lpthread -ldl -lrt

PG_CONFIG ?= pg_config
override with_llvm := no
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

.PHONY: core-test install-dds-runtime

core-test:
	$(MKDIR_P) build
	$(CXX) -std=c++17 -Wall -Wextra -Wpedantic -Werror \
		-Iinclude tests/control_core_test.cpp src/control_core.cpp \
		-o build/control_core_test
	./build/control_core_test

install: install-dds-runtime

install-dds-runtime:
	$(MKDIR_P) '$(DESTDIR)$(pkglibdir)'
	$(INSTALL_SHLIB) '$(UNITREE_ROOT)/thirdparty/lib/$(UNITREE_ARCH)/libddsc.so' \
		'$(DESTDIR)$(pkglibdir)/libddsc.so.0'
	$(INSTALL_SHLIB) '$(UNITREE_ROOT)/thirdparty/lib/$(UNITREE_ARCH)/libddsc.so' \
		'$(DESTDIR)$(pkglibdir)/libddsc.so'
	$(INSTALL_SHLIB) '$(UNITREE_ROOT)/thirdparty/lib/$(UNITREE_ARCH)/libddscxx.so' \
		'$(DESTDIR)$(pkglibdir)/libddscxx.so.0'
	$(INSTALL_SHLIB) '$(UNITREE_ROOT)/thirdparty/lib/$(UNITREE_ARCH)/libddscxx.so' \
		'$(DESTDIR)$(pkglibdir)/libddscxx.so'
