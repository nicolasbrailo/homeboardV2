BIN_NAME = ambience
SRCS = main.c config.c dbus.c dbus_helpers.c display.c slideshow.c \
       ../lib/drm_mgr/drm_mgr.c \
       ../lib/jpeg_render/jpeg_loader.c \
       ../lib/jpeg_render/img_render.c
CFLAGS += -I../lib/
LDFLAGS += -Wl,-Bstatic -ljson-c -Wl,-Bdynamic -lsystemd -ljpeg -lpthread

all: $(BIN_NAME)

include ../common.mk

SSH = ssh $(DEPLOY_TGT_HOST)
DBUS_POLICY = io.homeboard.Ambience.conf
DBUS_POLICY_DIR = /etc/dbus-1/system.d

deploy-config: config.json
	scp config.json $(DEPLOY_TGT_HOST):$(DEPLOY_TGT_DIR)/$(BIN_NAME).json

deploy-dbus-policy: $(DBUS_POLICY)
	scp $(DBUS_POLICY) $(DEPLOY_TGT_HOST):/tmp/$(DBUS_POLICY)
	$(SSH) 'sudo -S install -m 644 -o root -g root /tmp/$(DBUS_POLICY) $(DBUS_POLICY_DIR)/$(DBUS_POLICY) && rm /tmp/$(DBUS_POLICY) && sudo systemctl reload dbus'

next:
	$(SSH) 'busctl --system call io.homeboard.Ambience /io/homeboard/Ambience io.homeboard.Ambience1 Next'
