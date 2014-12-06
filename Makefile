# CPE464 Makefile for Program 3: fishnode L3
# Jacob Hladky
# Modified 11/23/14 for JSON server project
# Modified 12/05/14 for further expansion

CPP=g++
CPPFLAGS = -O3 -Wall -Werror -Wextra -Wno-unused-parameter -std=c++0x
OS = $(shell uname -s)
PROC = $(shell uname -m)
EXEC_SUFFIX = $(OS)-$(PROC)

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

http-server-$(EXEC_SUFFIX)-debug: http-server.cpp
	$(CPP) $(CPPFLAGS) -DDEBUG $(OSINC) $(OSLIB) $(OSFLAGS) $(ARCHFLAGS) $< -o $@

http-server-$(EXEC_SUFFIX): http-server.cpp
	$(CPP) $(CPPFLAGS) $(OSINC) $(OSLIB) $(OSFLAGS) $(ARCHFLAGS) $< -o $@

all: http-server-$(EXEC_SUFFIX)

clean:
	-rm -rf http-server-* *.o
