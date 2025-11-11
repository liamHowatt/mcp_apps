include $(APPDIR)/Make.defs


# Beeper

CSRCS += $(wildcard *.c)
CSRCS += $(wildcard sas_emoji/*.c)


# pdjson

CSRCS += pdjson/pdjson.c


# olm

CXXEXT := .cpp

# define MAJOR, MINOR, PATCH
include $(APPDIR)/mcp_apps/beeper/olm/common.mk
CXXFLAGS += -DOLMLIB_VERSION_MAJOR=$(MAJOR) -DOLMLIB_VERSION_MINOR=$(MINOR) \
            -DOLMLIB_VERSION_PATCH=$(PATCH)

CFLAGS +=   ${INCDIR_PREFIX}$(APPDIR)/mcp_apps/beeper/olm/include
CXXFLAGS += ${INCDIR_PREFIX}$(APPDIR)/mcp_apps/beeper/olm/include
CFLAGS +=   ${INCDIR_PREFIX}$(APPDIR)/mcp_apps/beeper/olm/lib
CXXFLAGS += ${INCDIR_PREFIX}$(APPDIR)/mcp_apps/beeper/olm/lib

# hacks
CXXFLAGS += ${INCDIR_PREFIX}$(APPDIR)/mcp_apps/beeper/include_local
CFLAGS   += -Wno-shadow -Wno-strict-prototypes
CXXFLAGS += -Wno-shadow -fpermissive

CSRCS +=   $(wildcard olm/src/*.c)
CSRCS +=   olm/lib/crypto-algorithms/sha256.c
CSRCS +=   olm/lib/crypto-algorithms/aes.c
CSRCS +=   olm/lib/curve25519-donna/curve25519-donna.c

CXXSRCS += $(wildcard olm/src/*.cpp)


include $(APPDIR)/Application.mk
