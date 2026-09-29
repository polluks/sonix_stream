# $VER: makefile 1.0 (29.09.2026)

VERSION     =   1

CC = ppc-morphos-gcc
CFLAGS = -noixemul -nostartfiles -O2 -fomit-frame-pointer -mno-prototype -mcpu=604e -mmultiple
CFLAGS += -W -Wall -Wpointer-arith -Wno-parentheses
LD = ppc-morphos-gcc
LDFLAGS = -nostartfiles -noixemul
LIBS = -labox -lm -lmath
STRIP = ppc-morphos-strip --remove-section .comment
VER := $(shell grep "define VERS " class_version.h | cut -d " " -f 5 | tr -d "\042" )
OUTPUT = sonix.stream
COMMON = ../common
OBJS = sonix.stream.o $(COMMON)/capture.o $(COMMON)/psdusb.o $(COMMON)/bayer.o

#===============================================================================

.PHONY: all  clean install

all: $(OUTPUT)

clean:
	-rm -rf $(OBJS) *.bak *.s *.db $(OUTPUT) recognize
	-rm -f capture.o psdusb.o bayer.o

install: all
	mkdir -p /SYS/MorphOS/Classes/Multimedia
	cp $(OUTPUT) /SYS/MorphOS/Classes/Multimedia/
	-flushlib $(OUTPUT)

#===============================================================================

$(OUTPUT).db: $(OBJS) recognize
	$(LD) $(LDFLAGS) $(OBJS) $(LIBS) -o $(OUTPUT).db
	ppc-morphos-objcopy --add-section .dtcode=recognize $(OUTPUT).db
	ppc-morphos-objcopy --set-section-flags .dtcode=readonly $(OUTPUT).db

$(OUTPUT): $(OUTPUT).db
	$(STRIP) $(OUTPUT).db -o $(OUTPUT)

recognize.db: recognize.c class_version.h sonix.stream.h ../sonixwire.h
	ppc-morphos-gcc -noixemul -s -O2 -nostdlib -o recognize.db recognize.c -labox

recognize: recognize.db
	ppc-morphos-objcopy -R .comment recognize.db recognize

#===============================================================================

sonix.stream.o: sonix.stream.c sonix.stream.h class_version.h ../sonixwire.h $(COMMON)/capture.h $(COMMON)/psdusb.h $(COMMON)/bayer.h
	$(CC) $(CFLAGS) -I$(COMMON) -c $<

$(COMMON)/%.o: $(COMMON)/%.c
	$(CC) $(CFLAGS) -I$(COMMON) -c -o $@ $<

#===============================================================================
