include $(APPDIR)/Make.defs

# peanut_gb

CSRCS = peanut_gb.c

ifeq ($(CONFIG_MCP_APPS_PEANUT_GB_SOUND),y)
	CSRCS += Peanut-GB/examples/sdl2/minigb_apu/minigb_apu.c
	CFLAGS += -DMINIGB_APU_AUDIO_FORMAT_S16SYS
endif

include $(APPDIR)/Application.mk
