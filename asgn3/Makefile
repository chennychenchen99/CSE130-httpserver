#-------------------------
# Daniel Chen, Daniel Xiong
# dchen61, dxiong5
# CSE 130
# Asgn1 - Makefile
#-------------------------

SOURCE = httpserver.cpp
OBJECT = httpserver.o
EXECUTABLE = httpserver

all: $(EXECUTABLE)

$(EXECUTABLE): $(OBJECT) $(SOURCE)
	g++ -c $(SOURCE)
	g++ -std=gnu++11 -Wall -Wextra -Wpedantic -Wshadow -o $(EXECUTABLE) $(OBJECT)

clean:
	rm -f *.o $(EXECUTABLE)