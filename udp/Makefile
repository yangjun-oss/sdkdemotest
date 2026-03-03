CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra

all: udp_server udp_client

udp_server: udp_server.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

udp_client: udp_client.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f udp_server udp_client

.PHONY: all clean
