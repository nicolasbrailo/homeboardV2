BIN_NAME = display-mgr
SRCS = main.c drm.c drm_lease.c sock.c run_state.c
LDFLAGS += -static -ldrm

all: $(BIN_NAME)

include ../common.mk

install_sysroot_deps:
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/libd/libdrm/libdrm-dev_2.4.131-1+rpi1_armhf.deb

set_perms:
	ssh "$(DEPLOY_TGT_HOST)" sudo usermod -aG video batman
	ssh "$(DEPLOY_TGT_HOST)" "sudo setcap cap_sys_admin+ep display-mgr"

