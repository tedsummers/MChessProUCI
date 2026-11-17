CC      = gcc
CFLAGS  = -m32 -O3 -Wall -Wextra -std=c11 -s
LDFLAGS = -pthread

all: MChessProUCI

MChessProUCI: mchesspro.c mcpro_data.h
	$(CC) $(CFLAGS) -o MChessProUCI mchesspro.c $(LDFLAGS)

windows: MChessProUCI.exe

MChessProUCI.exe: mchesspro.c mcpro_data.h
	zig cc -target x86-windows-gnu -O3 -Wall -Wextra -std=c11 -s -o MChessProUCI.exe mchesspro.c

clean:
	rm -f MChessProUCI MChessProUCI.exe

.PHONY: all windows clean
