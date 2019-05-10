CC=gcc
INC=../inc
all: 
	terminus
terminus: terminus.c
	$(CC) -o $@ $< -I$(INC) -g -Wall
clean:
	rm -f terminus
