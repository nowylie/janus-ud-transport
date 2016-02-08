JANUS=/opt/janus
NAME=janus-unix-dgram
LINUX_NAME=$(NAME).so
OSX_NAME=$(NAME).0.dylib

CC=gcc
CFLAGS=-std=c99 -fpic -I. -I$(JANUS)/include `pkg-config --cflags glib-2.0 jansson` -D_POSIX_C_SOURCE=200112L -c -g

linux: unix-dgram
	$(CC) -shared -o $(LINUX_NAME) unix-dgram.o -lpthread `pkg-config --libs glib-2.0 jansson`

unix-dgram: unix-dgram.c
	$(CC) $(CFLAGS) unix-dgram.c

install:
	cp $(LINUX_NAME) $(JANUS)/lib/janus/transports/
