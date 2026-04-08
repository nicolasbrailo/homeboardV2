.PHONY: clean xcompile-start xcompile-end

DEPLOY_TGT_HOST=batman@10.0.0.114
DEPLOY_TGT_DIR=/home/batman/

XCOMPILE+=\
	-target arm-linux-gnueabihf \
	-mcpu=arm1176jzf-s \
	--sysroot ~/src/xcomp-rpiz-env/mnt/  \
	-isystem ~/src/xcomp-rpiz-env/mnt/usr/include/libdrm

CFLAGS+=\
	$(XCOMPILE) \
	-fdiagnostics-color=always \
	-ffunction-sections -fdata-sections \
	-ggdb -O3 \
	-std=gnu99 \
	-Wall -Werror -Wextra -Wpedantic \
	-Wendif-labels \
	-Wfloat-equal \
	-Wformat=2 \
	-Wimplicit-fallthrough \
	-Winit-self \
	-Winvalid-pch \
	-Wmissing-field-initializers \
	-Wmissing-include-dirs \
	-Wno-strict-prototypes \
	-Wno-unused-function \
	-Wno-unused-parameter \
	-Woverflow \
	-Wpointer-arith \
	-Wredundant-decls \
	-Wstrict-aliasing=2 \
	-Wundef \
	-Wuninitialized \

xcompile-start:
	../rpiz-xcompile/mount_rpy_root.sh ~/src/xcomp-rpiz-env

xcompile-end:
	../rpiz-xcompile/umount_rpy_root.sh ~/src/xcomp-rpiz-env

clean:
	rm -rf build

build/%.o: %.c
	mkdir -p build
	@if [ ! -d ~/src/xcomp-rpiz-env/mnt/lib/raspberrypi-sys-mods ]; then \
		echo "xcompiler sysroot not detected, try 'make xcompile-start'"; \
		exit 1; \
	fi ;
	clang $(CFLAGS) $< -c -o $@

.phony: $(BIN_NAME)
$(BIN_NAME):build/$(BIN_NAME)
build/$(BIN_NAME): $(patsubst %.c,build/%.o,$(SRCS))
	clang $(CFLAGS) $^ $(LDFLAGS) -o $@

deploy: build/$(BIN_NAME)
	scp ./build/$(BIN_NAME) $(DEPLOY_TGT_HOST):$(DEPLOY_TGT_DIR)

