# CPE464 Makefile for Program 3: fishnode L3
# Jacob Hladky
# Modified 11/23/14 for JSON server project

CC=gcc
CPP=g++
CFLAGS = -O3 -Wall -Werror -pedantic -Wextra -Wno-unused-parameter -std=c99
CPPFLAGS = -O3 -Wall -Werror -Wextra -Wno-unused-parameter -std=c++0x
OS = $(shell uname -s)
PROC = $(shell uname -m)
EXEC_SUFFIX=$(OS)-$(PROC)

ifeq ("$(OS)", "SunOS")
	OSLIB=-L/opt/csw/lib -R/opt/csw/lib -lsocket -lnsl
	OSINC=-I/opt/csw/include
	OSFLAGS=-DSOLARIS
else
ifeq ("$(OS)", "Darwin")
	OSLIB=
	OSINC=
	OSFLAGS=-DDARWIN -Wno-deprecated-writable-strings -Wno-inline-new-delete
else
	OSLIB=
	OSINC=
	OSFLAGS=-DLINUX -D_BSD_SOURCE
endif
endif

ifeq ("$(PROC)", "i686")
	ARCHFLAGS=
else
#	ARCHFLAGS=-m32
	ARCHFLAGS=
endif

json-server-$(EXEC_SUFFIX): json-server.cpp smartalloc-$(EXEC_SUFFIX).o 
	$(CPP) $(CPPFLAGS) $(OSINC) $(OSLIB) $(OSFLAGS) $(ARCHFLAGS) $< smartalloc-$(EXEC_SUFFIX).o -o $@

all:  http-server-$(EXEC_SUFFIX)

smartalloc-$(EXEC_SUFFIX).o: smartalloc.c
	$(CC) $(CFLAGS) $(OSINC) $(OSFLAGS) $(ARCHFLAGS) -DSMARTALLOC_PEDANTIC -c $< -o $@

clean:
	-rm -rf json-server-* *.o
