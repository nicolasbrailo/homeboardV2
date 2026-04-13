BIN_NAME = display-mgr
SRCS = main.c drm.c dbus.c
LDFLAGS += -Wl,-Bstatic -ldrm -Wl,-Bdynamic -lsystemd -lpthread

all: $(BIN_NAME)

include ../common.mk

SSH = ssh $(DEPLOY_TGT_HOST)
DBUS_POLICY = io.homeboard.Display.conf
DBUS_POLICY_DIR = /etc/dbus-1/system.d

install_sysroot_deps:
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/libd/libdrm/libdrm-dev_2.4.131-1+rpi1_armhf.deb
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/s/systemd/libsystemd-dev_252.39-1~deb12u1+rpi1_armhf.deb
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/libc/libcap2/libcap-dev_2.66-4+deb12u2_armhf.deb

deploy-dbus-policy: $(DBUS_POLICY)
	scp $(DBUS_POLICY) $(DEPLOY_TGT_HOST):/tmp/$(DBUS_POLICY)
	$(SSH) 'sudo -S install -m 644 -o root -g root /tmp/$(DBUS_POLICY) $(DBUS_POLICY_DIR)/$(DBUS_POLICY) && rm /tmp/$(DBUS_POLICY) && sudo systemctl reload dbus'

set_perms:
	ssh "$(DEPLOY_TGT_HOST)" sudo usermod -aG video batman
	ssh "$(DEPLOY_TGT_HOST)" "sudo setcap cap_sys_admin+ep display-mgr"
