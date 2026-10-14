CXX ?= g++
CXXFLAGS ?= -O2 -g
CPPFLAGS ?=
LDFLAGS ?=
PREFIX ?= /usr

.PHONY: all clean test install deb
all: cec

cec: cec.cpp musiccast_network.cpp musiccast_network.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++17 -Wall -Wextra -pthread cec.cpp musiccast_network.cpp $(LDFLAGS) -o $@ -lcec -lcurl -lboost_program_options

tests/network_test: tests/network_test.cpp musiccast_network.cpp musiccast_network.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++17 -Wall -Wextra -I. tests/network_test.cpp musiccast_network.cpp $(LDFLAGS) -o $@ -lcurl

tests/bridge_test: tests/bridge_test.cpp cec.cpp musiccast_network.cpp musiccast_network.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++17 -Wall -Wextra -pthread -I. tests/bridge_test.cpp musiccast_network.cpp $(LDFLAGS) -o $@ -lcec -lcurl -lboost_program_options

test: cec tests/network_test tests/bridge_test
	./tests/network_test
	python3 -m unittest discover -s tests -v

install: cec
	install -Dm755 cec $(DESTDIR)$(PREFIX)/bin/cec-musiccast-bridge
	install -Dm644 debian/cec-musiccast-bridge.conf $(DESTDIR)/etc/cec-musiccast-bridge.conf

deb:
	./scripts/build-deb.sh

clean:
	$(RM) cec tests/network_test tests/bridge_test
