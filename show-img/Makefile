BIN_NAME = show-img
SRCS = main.c jpeg_loader.c img_render.c ../lib/drm_mgr/drm_mgr.c
CFLAGS += -I../lib/drm_mgr
LDFLAGS += -static -ljpeg

all: $(BIN_NAME)

include ../common.mk

install_sysroot_deps:
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/libj/libjpeg-turbo/libjpeg62-turbo-dev_2.1.5-4_armhf.deb
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/libj/libjpeg-turbo/libjpeg62-turbo_2.1.5-4_armhf.deb

