include $(APPDIR)/Make.defs

# MCP Forth built-in application info

PROGNAME = $(CONFIG_MCP_APPS_MCP_FORTH_PROGNAME)
PRIORITY = $(CONFIG_MCP_APPS_MCP_FORTH_PRIORITY)
STACKSIZE = $(CONFIG_MCP_APPS_MCP_FORTH_STACKSIZE)
MODULE = $(CONFIG_MCP_APPS_MCP_FORTH)

# MCP Forth

ifneq ($(CONFIG_SCHED_THREAD_LOCAL),y)
	CFLAGS += ${DEFINE_PREFIX}M4_NO_TLS ${DEFINE_PREFIX}M4_NO_THREAD
endif

MAINSRC = mcp_forth.c
CSRCS += mcp_forth/mcp_forth.c
CSRCS += mcp_forth/compile.c
CSRCS += mcp_forth/vm_backend.c
CSRCS += mcp_forth/vm_engine.c

ifneq ($(CONFIG_MCP_APPS_MCP_FORTH_NATIVE_NONE),y)
	CSRCS += mcp_forth/elf_nuttx.c
endif

ifeq ($(CONFIG_MCP_APPS_MCP_FORTH_NATIVE_X86_32),y)

mcp_forth/x86-32_backend.c: mcp_forth/x86-32_backend_generated.h

mcp_forth/x86-32_backend_generated.c: mcp_forth/x86-32_backend_generated.phony
mcp_forth/x86-32_backend_generated.h: mcp_forth/x86-32_backend_generated.phony

mcp_forth/x86-32_backend_generated.phony: mcp_forth/x86_32_backend_generator.py mcp_forth/nasm.py
	bash -c 'cd mcp_forth && python3 x86_32_backend_generator.py'
	touch mcp_forth/x86-32_backend_generated.phony

mcp_forth/x86-32_engine_asm.o: mcp_forth/x86-32_engine_asm.s
	nasm -felf32 -o mcp_forth/x86-32_engine_asm.o mcp_forth/x86-32_engine_asm.s

clean::
	$(call DELFILE, mcp_forth/x86-32_backend_generated.phony)
	$(call DELFILE, mcp_forth/x86-32_backend_generated.c)
	$(call DELFILE, mcp_forth/x86-32_backend_generated.h)

distclean:: clean

CSRCS += mcp_forth/x86-32_backend.c
CSRCS += mcp_forth/x86-32_engine.c
CSRCS += mcp_forth/x86-32_backend_generated.c
EXTOBJS += mcp_forth/x86-32_engine_asm.o

endif

CSRCS += mcp_forth/runtime_io.c
CSRCS += mcp_forth/runtime_string.c
CSRCS += mcp_forth/runtime_time.c
CSRCS += mcp_forth/runtime_assert.c
CSRCS += mcp_forth/runtime_threadutil.c
CSRCS += bindings/runtime_mcpd.c
CSRCS += bindings/runtime_spi.c
CSRCS += bindings/runtime_input.c
CSRCS += bindings/runtime_mcp_lvgl.c
CSRCS += bindings/runtime_unix.c
CSRCS += bindings/runtime_malloc.c

include $(APPDIR)/Application.mk
