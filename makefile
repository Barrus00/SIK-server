CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -O2
LDFLAGS =
LIBS = -lstdc++fs

SRCS = server.cpp err.cpp
OBJS = $(subst .cpp,.o, $(SRCS))

all: serwer

serwer: $(OBJS)
		$(CXX) $(LDFLAGS) -o serwer $(OBJS) $(LIBS)

err.o: err.cpp err.h
	   $(CXX) $(CXXFLAGS) -c $<

main.o: server.cpp err.h
		$(CXX) $(CXXFLAGS) -c $<

clean:
	rm -f $(OBJS) serwer
