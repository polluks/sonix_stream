# $VER: makefile 1.0 (29.09.2026)

SUBDIRS = stream demuxer

#===============================================================================

.PHONY: all clean install

all:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d || exit 1; done

clean:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d clean || exit 1; done

install: all
	@for d in $(SUBDIRS); do $(MAKE) -C $$d install || exit 1; done

#===============================================================================
