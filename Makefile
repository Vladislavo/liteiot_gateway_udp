CC 		= gcc

INCLUDES 	= -I/usr/include/postgresql
LFLAGS 		= -Llib

LDIR		= lib
LIBS 		= $(LDIR)/gateway_protocol/gateway_protocol

LIBD		= -pthread -lpq

SRC 		= src/gateway.c

OBJ 		= obj

MAIN 		= gateway

.PHONY: depend clean

$(LDIR)/gateway_protocol/gateway_protocol.o:
	$(CC) -c $(LIBS)/gateway_protocol.c -o $(OBJ)/gateway_protocol.o -I$(LIBS)
	$(CC) $(SRC) $(OBJ)/gateway_protocol.o -o $(MAIN) -I$(LIBS) $(LIBD) $(INCLUDES)


all: $(MAIN)
	@echo Compiling gateway project

$(MAIN): $(OBJ)/gateway_protocol.o
	$(CC) $(CFLAGS) $(INCLUDES) -o $(MAIN) $(OBJS) $(LFLAGS) $(LIBS)

.c.o:
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	$(RM) *.o *~ $(MAIN)

depend: $(SRCS)
	makedepend $(INCLUDES) $^

# line needed by makedepend
