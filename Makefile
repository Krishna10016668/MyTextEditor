CC = gcc
CFLAGS = -Wall -Wextra -pedantic -std=c99

MyTextEditor.exe: MyTextEditor.c
	 $(CC) $(CFLAGS) MyTextEditor.c -o MyTextEditor 

clean:
	rm -f MyTextEditor	 