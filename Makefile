SUBDIRS := $(filter-out rpiz-xcompile, $(patsubst %/,%,$(wildcard */)))

.PHONY: all clean $(SUBDIRS)

all: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@

clean:
	@for dir in $(SUBDIRS); do $(MAKE) -C $$dir clean; done

deploy:
	@for dir in $(SUBDIRS); do $(MAKE) -C $$dir deploy; done
