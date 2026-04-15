BIN_NAME = ambience
SRCS = main.c config.c display.c slideshow.c \
       ../lib/drm_mgr/drm_mgr.c \
       ../lib/jpeg_render/jpeg_loader.c \
       ../lib/jpeg_render/img_render.c
CFLAGS += -I../lib/
LDFLAGS += -Wl,-Bstatic -ljson-c -Wl,-Bdynamic -lsystemd -ljpeg -lpthread

all: $(BIN_NAME)

include ../common.mk

deploy-config: config.json
	scp config.json $(DEPLOY_TGT_HOST):$(DEPLOY_TGT_DIR)/$(BIN_NAME).json
