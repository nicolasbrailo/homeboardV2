BIN_NAME = photo-provider
SRCS = main.c config.c www_session.c cache.c dbus.c
LDFLAGS += -Wl,-Bstatic -ljson-c -Wl,-Bdynamic -lcurl -lsystemd -lpthread

all: $(BIN_NAME)

include ../common.mk

SSH = ssh $(DEPLOY_TGT_HOST)
DBUS_POLICY = io.homeboard.PhotoProvider.conf
DBUS_POLICY_DIR = /etc/dbus-1/system.d

install_sysroot_deps:
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/j/json-c/libjson-c-dev_0.18+ds-3_armhf.deb
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/s/systemd/libsystemd-dev_252.39-1~deb12u1+rpi1_armhf.deb
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/o/openldap/libldap-2.5-0_2.5.13+dfsg-5+rpi1_armhf.deb
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/c/curl/libcurl4_7.88.1-10+rpi1+deb12u14_armhf.deb
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/c/curl/libcurl4-openssl-dev_7.88.1-10+rpi1+deb12u14_armhf.deb

deploy-config: config.json
	scp config.json $(DEPLOY_TGT_HOST):$(DEPLOY_TGT_DIR)/$(BIN_NAME).json

deploy-dbus-policy: $(DBUS_POLICY)
	scp $(DBUS_POLICY) $(DEPLOY_TGT_HOST):/tmp/$(DBUS_POLICY)
	$(SSH) 'sudo -S install -m 644 -o root -g root /tmp/$(DBUS_POLICY) $(DBUS_POLICY_DIR)/$(DBUS_POLICY) && rm /tmp/$(DBUS_POLICY) && sudo systemctl reload dbus'
