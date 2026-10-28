include $(APPDIR)/Make.defs

# libpng

LIBPNG_NOHWOPT = -DPNG_ARM_NEON_OPT=0 -DPNG_MIPS_MSA_OPT=0 \
                 -DPNG_POWERPC_VSX_OPT=0 -DPNG_INTEL_SSE_OPT=0 \
                 -DPNG_RISCV_RVV_OPT=0

CFLAGS += $(LIBPNG_NOHWOPT)

CSRCS = libpng/png.c libpng/pngerror.c libpng/pngget.c libpng/pngmem.c libpng/pngpread.c \
        libpng/pngread.c libpng/pngrio.c libpng/pngrtran.c libpng/pngrutil.c libpng/pngset.c \
        libpng/pngtrans.c libpng/pngwio.c libpng/pngwrite.c libpng/pngwtran.c libpng/pngwutil.c

include $(APPDIR)/Application.mk
