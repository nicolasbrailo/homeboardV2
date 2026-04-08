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

