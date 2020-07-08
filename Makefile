CC 		= gcc

INCLUDES 	= -I/usr/include/postgresql -I$(LDIR)/gateway_protocol -I$(LDIR)/base64 -I$(LDIR)/task_queue
LFLAGS 		= -Llib

LDIR		= lib
LIBD		= -pthread -lpq

SRC 		= src/gateway.c

OBJ 		= obj

MAIN 		= gateway

.PHONY: depend clean

$(LDIR)/gateway_protocol/gateway_protocol.o:
	$(CC) -c $(LDIR)/gateway_protocol/gateway_protocol.c -o $(OBJ)/gateway_protocol.o -I$(LDIR)/gateway_protocol
	$(CC) -c $(LDIR)/base64/base64.c -o $(OBJ)/base64.o -I$(LDIR)/base64
	$(CC) -c $(LDIR)/task_queue/task_queue.c -o $(OBJ)/task_queue.o -I$(LDIR)/task_queue
	$(CC) $(SRC) $(OBJ)/gateway_protocol.o $(OBJ)/base64.o $(OBJ)/task_queue.o -o $(MAIN) $(LIBD) $(INCLUDES)


all: $(MAIN)
	@echo Compiling gateway project

$(MAIN): $(OBJ)/gateway_protocol.o $(OBJ)/base64.o $(OBJ)/task_queue.o
	$(CC) $(CFLAGS) $(INCLUDES) -o $(MAIN) $(OBJS) $(LFLAGS)

.c.o:
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	$(RM) *.o *~ $(MAIN)

depend: $(SRCS)
	makedepend $(INCLUDES) $^

# line needed by makedepend
