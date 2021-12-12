CFLAGS=-O
LDLIBS=-lbcm2835

all: gpio-server

clean:
	$(RM) gpio-server
