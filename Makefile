default: mancsrv
all: mancsrv

mancsrv: mancsrv.c
	gcc -Wall -std=gnu99 -g mancsrv.c -o mancsrv

clean:
	rm -rf mancsrv