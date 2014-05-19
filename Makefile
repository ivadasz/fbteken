CC = gcc
CFLAGS = -O2 -march=native -pipe
CFLAGS += -I /usr/local/include -I/usr/local/include/libdrm -I/usr/local/include/libkms -I/usr/local/include/freetype2
LDFLAGS = -s
LDFLAGS += -lpthread -lutil -L/usr/local/lib -ldrm -lkms -lfreetype

# settings for finding the header files for libteken
CFLAGS += -I/var/tmp/teken
# specify the settings for linking against libteken
LDFLAGS += -L/var/tmp/teken/libteken
LDFLAGS += -lteken -Wl,-rpath=/var/tmp/teken/libteken

OBJECTS = fbteken.o rop32.o

all: fbteken

fbteken: $(OBJECTS)
	$(CC) -o $@ $(OBJECTS) $(LDFLAGS)

.c.o:
	$(CC) -c $(CFLAGS) $<

clean:
	rm -f fbteken $(OBJECTS)

.PHONY: clean
