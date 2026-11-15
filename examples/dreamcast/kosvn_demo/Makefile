TARGET = kosvn_demo.elf
OBJS = main.o romdisk.o
CDI = kosvn_demo.cdi
MKDCDISC = /h/DC/tools/mkdcdisc_bin/mkdcdisc_win/mkdcdisc64.exe
KOS_ROMDISK_DIR = romdisk
KOS_CFLAGS += -I$(KOS_BASE)/addons/include -O3 -fomit-frame-pointer -ffast-math -Wall -Wextra

all: rm-elf $(TARGET)

include $(KOS_BASE)/Makefile.rules

clean: rm-elf
	-rm -f $(OBJS)

rm-elf:
	-rm -f $(TARGET) $(CDI) romdisk.*

$(TARGET): $(OBJS)
	kos-cc -o $(TARGET) $(OBJS) -L$(KOS_BASE)/addons/lib/$(KOS_ARCH) -lkosvn

cdi: $(TARGET)
	$(MKDCDISC) -e $(TARGET) -o $(CDI) -n "KOSVN_DEMO"

run: $(TARGET)
	$(KOS_LOADER) $(TARGET)
