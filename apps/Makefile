ALL  = dump-config test_app #image

GTK_CFLAGS = `pkg-config --cflags gtk+-2.0`
GTK_LIBS = `pkg-config --libs gtk+-2.0`
CFLAGS=-g -I../driver $(GTK_CFLAGS)
CC=gcc

all: $(ALL)

clean:
	rm -f *.o $(ALL)

test_app: lib.o test_app.o dinter.o demux.o
	$(CC) -g -o test_app lib.o test_app.o dinter.o demux.o

image: image.o uart.o lib.o demux.o
	$(CC) -g -o image image.o uart.o lib.o demux.o $(GTK_LIBS)  -lpthread

