BIN_NAME = occupancy-sensor
CFLAGS += -I../lib/
SRCS = main.c ld2410s.c ld2410s_uart/ld2410s_uart.c ld2410s_uart/session.c ld2410s_uart/transport.c ../lib/rpigpio/rpigpio.c
LDFLAGS += -Wl,-Bstatic -ljson-c -Wl,-Bdynamic -lsystemd -lpthread

all: $(BIN_NAME)

include ../common.mk

BOOT_CONFIG = /boot/firmware/config.txt
CMDLINE = /boot/firmware/cmdline.txt
SSH = ssh $(DEPLOY_TGT_HOST)

install_sysroot_deps:
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/j/json-c/libjson-c-dev_0.18+ds-3_armhf.deb
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/s/systemd/libsystemd-dev_252.39-1~deb12u1+rpi1_armhf.deb
	../rpiz-xcompile/add_sysroot_pkg.sh ~/src/xcomp-rpiz-env http://raspbian.raspberrypi.com/raspbian/pool/main/libc/libcap2/libcap-dev_2.66-4+deb12u2_armhf.deb

DBUS_POLICY = io.homeboard.Occupancy.conf
DBUS_POLICY_DIR = /etc/dbus-1/system.d

deploy-dbus-policy: $(DBUS_POLICY)
	scp $(DBUS_POLICY) $(DEPLOY_TGT_HOST):/tmp/$(DBUS_POLICY)
	$(SSH) 'sudo -S install -m 644 -o root -g root /tmp/$(DBUS_POLICY) $(DBUS_POLICY_DIR)/$(DBUS_POLICY) && rm /tmp/$(DBUS_POLICY) && sudo systemctl reload dbus'

.PHONY: dbus-listen
dbus-listen:
	$(SSH) "dbus-monitor --system \"type='signal',interface='io.homeboard.Occupancy1'\""

.PHONY: config-target
config-target:
	$(SSH) 'grep -q "^dtoverlay=disable-bt" $(BOOT_CONFIG) || echo "dtoverlay=disable-bt" | sudo tee -a $(BOOT_CONFIG) > /dev/null'
	$(SSH) 'grep -q "^enable_uart=1" $(BOOT_CONFIG) || echo "enable_uart=1" | sudo tee -a $(BOOT_CONFIG) > /dev/null'
	$(SSH) 'sudo systemctl disable serial-getty@ttyAMA0.service'
	$(SSH) 'sudo systemctl stop serial-getty@ttyAMA0.service || true'
	$(SSH) 'if grep -q "console=serial0" $(CMDLINE); then sudo sed -i "s/ *console=serial0,[0-9]*//" $(CMDLINE); echo "Removed serial console from $(CMDLINE)"; fi'
	$(SSH) 'sudo usermod -aG dialout $$USER'


