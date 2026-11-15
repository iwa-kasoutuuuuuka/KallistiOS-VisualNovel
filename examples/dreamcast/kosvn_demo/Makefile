TARGET = kosvn_demo.elf
OBJS = main.o romdisk.o
KOS_ROMDISK_DIR = romdisk
KOS_CFLAGS += -I$(KOS_BASE)/addons/include -Wall -Wextra

all: rm-elf $(TARGET)

include $(KOS_BASE)/Makefile.rules

clean: rm-elf
	-rm -f $(OBJS)

rm-elf:
	-rm -f $(TARGET) romdisk.*

$(TARGET): $(OBJS)
	kos-cc -o $(TARGET) $(OBJS) -L$(KOS_BASE)/addons/lib/$(KOS_ARCH) -lkosvn

run: $(TARGET)
	$(KOS_LOADER) $(TARGET)
