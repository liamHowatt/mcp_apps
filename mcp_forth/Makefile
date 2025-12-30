include $(APPDIR)/Make.defs

# MCP Forth built-in application info

PROGNAME = $(CONFIG_MCP_APPS_MCP_FORTH_PROGNAME)
PRIORITY = $(CONFIG_MCP_APPS_MCP_FORTH_PRIORITY)
STACKSIZE = $(CONFIG_MCP_APPS_MCP_FORTH_STACKSIZE)
MODULE = $(CONFIG_MCP_APPS_MCP_FORTH)

# MCP Forth

MCP_FORTH_GENERATOR_OPTS = MAX_CALLBACKS=$(CONFIG_MCP_APPS_MCP_FORTH_MAX_CALLBACKS)

ifneq ($(CONFIG_SCHED_THREAD_LOCAL),y)
	MCP_FORTH_GENERATOR_OPTS +=  NO_TLS  NO_THREAD
endif

mcp_forth/mcp_forth_generated.h: mcp_forth/mcp_forth_generated.phony
mcp_forth/mcp_forth_generated.mac: mcp_forth/mcp_forth_generated.phony

mcp_forth/mcp_forth_generated.phony: mcp_forth/mcp_forth_generator.py
	bash -c 'cd mcp_forth && python3 mcp_forth_generator.py $(MCP_FORTH_GENERATOR_OPTS)'
	touch mcp_forth/mcp_forth_generated.phony

mcp_forth/mcp_forth.h: mcp_forth/mcp_forth_generated.h
mcp_forth/x86-32_engine_asm.s: mcp_forth/mcp_forth_generated.mac
mcp_forth/esp32s3_engine_asm.S: mcp_forth/mcp_forth_generated.h
mcp_forth/x86_32_backend_generator.py: mcp_forth/backend_generator.py
mcp_forth/esp32s3_backend_generator.py: mcp_forth/backend_generator.py mcp_forth/esp32s3.h

MAINSRC = mcp_forth.c
CSRCS += mcp_forth/mcp_forth.c
CSRCS += mcp_forth/compile.c
CSRCS += mcp_forth/vm_backend.c
CSRCS += mcp_forth/vm_engine.c

samples_romfs_img.c: mcp_forth/forth_programs/
	genromfs -d mcp_forth/forth_programs/ -x lvgl -f m4_samples_romfs.img
	echo '#include <nuttx/config.h>' > samples_romfs_img.c
	echo '#ifdef CONFIG_MCP_APPS_MCP_FORTH_SAMPLES_ROMFS' >> samples_romfs_img.c
	echo -n 'const ' >> samples_romfs_img.c
	xxd -i m4_samples_romfs.img >> samples_romfs_img.c
	echo '#endif' >> samples_romfs_img.c
	rm m4_samples_romfs.img

CSRCS += samples_romfs_img.c

ifneq ($(CONFIG_MCP_APPS_MCP_FORTH_NATIVE_NONE),y)
	CSRCS += mcp_forth/elf_nuttx.c
endif

ifeq ($(CONFIG_MCP_APPS_MCP_FORTH_NATIVE_X86_32),y)

mcp_forth/x86-32_backend.c: mcp_forth/x86_32_backend_generated.h

mcp_forth/x86_32_backend_generated.c: mcp_forth/x86_32_backend_generated.phony
mcp_forth/x86_32_backend_generated.h: mcp_forth/x86_32_backend_generated.phony

mcp_forth/x86_32_backend_generated.phony: mcp_forth/x86_32_backend_generator.py
	bash -c 'cd mcp_forth && python3 x86_32_backend_generator.py'
	touch mcp_forth/x86_32_backend_generated.phony

mcp_forth/x86-32_engine_asm.o: mcp_forth/x86-32_engine_asm.s
	nasm -felf32 -o mcp_forth/x86-32_engine_asm.o mcp_forth/x86-32_engine_asm.s

clean::
	$(call DELFILE, mcp_forth/x86_32_backend_generated.phony)
	$(call DELFILE, mcp_forth/x86_32_backend_generated.c)
	$(call DELFILE, mcp_forth/x86_32_backend_generated.h)

CSRCS += mcp_forth/x86-32_backend.c
CSRCS += mcp_forth/x86-32_engine.c
CSRCS += mcp_forth/x86_32_backend_generated.c
EXTOBJS += mcp_forth/x86-32_engine_asm.o

endif
ifeq ($(CONFIG_MCP_APPS_MCP_FORTH_NATIVE_ESP32S3),y)

mcp_forth/esp32s3_backend.c: mcp_forth/esp32s3_backend_generated.h

mcp_forth/esp32s3_backend_generated.c: mcp_forth/esp32s3_backend_generated.phony
mcp_forth/esp32s3_backend_generated.h: mcp_forth/esp32s3_backend_generated.phony

mcp_forth/esp32s3_backend_generated.phony: mcp_forth/esp32s3_backend_generator.py
	bash -c 'cd mcp_forth && python3 esp32s3_backend_generator.py'
	touch mcp_forth/esp32s3_backend_generated.phony

clean::
	$(call DELFILE, mcp_forth/esp32s3_backend_generated.*)

CSRCS += mcp_forth/esp32s3_backend.c
CSRCS += mcp_forth/esp32s3_engine.c
CSRCS += mcp_forth/esp32s3_backend_generated.c
ASRCS += mcp_forth/esp32s3_engine_asm.S

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
CSRCS += bindings/runtime_mount.c

clean::
	$(call DELFILE, mcp_forth/mcp_forth_generated.* m4_samples_romfs.img samples_romfs_img.c)

distclean:: clean

include $(APPDIR)/Application.mk
