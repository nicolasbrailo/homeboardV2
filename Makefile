SUBDIRS := $(filter-out rpiz-xcompile stockimgs lib, $(patsubst %/,%,$(wildcard */)))

.PHONY: all clean format $(SUBDIRS)

all: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@

clean:
	@for dir in $(SUBDIRS); do $(MAKE) -C $$dir clean; done

deploy:
	@for dir in $(SUBDIRS); do $(MAKE) -C $$dir deploy; done

format:
	find lib -name '*.c' -o -name '*.h' | xargs clang-format -i
	@for dir in $(SUBDIRS); do $(MAKE) -C $$dir format; done

deploy-stock-imgs:
	scp -r ./stockimgs 10.0.0.114:/home/batman

patch-target-config:
	@echo "Will patch a bunch of settings and permissions that are needed for the project to run. Check each specific project for details on what they do"
	ssh "$(DEPLOY_TGT_HOST)" sudo raspi-config nonint do_spi 0
	ssh "$(DEPLOY_TGT_HOST)" sudo dtparam spi=on
	ssh "$(DEPLOY_TGT_HOST)" sudo usermod -aG gpio,spi '$$USER'
	ssh "$(DEPLOY_TGT_HOST)" sudo usermod -aG video '$$USER'
	ssh "$(DEPLOY_TGT_HOST)" sudo setcap cap_sys_admin+ep display-mgr

