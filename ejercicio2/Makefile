# --------------------------------------------------
# Makefile para compilar servidor y cliente Ahorcado
# --------------------------------------------------

CC = gcc
CFLAGS = -Wall -pthread
CLIENT_SRC = cliente.c
SERVER_SRC = servidor.c
CLIENT_BIN = cliente
SERVER_BIN = servidor

all: $(SERVER_BIN) $(CLIENT_BIN)

$(SERVER_BIN): $(SERVER_SRC)
	$(CC) $(CFLAGS) -o $(SERVER_BIN) $(SERVER_SRC)

$(CLIENT_BIN): $(CLIENT_SRC)
	$(CC) -Wall -o $(CLIENT_BIN) $(CLIENT_SRC)

clean:
	rm -f $(SERVER_BIN) $(CLIENT_BIN)
