# The client is one header; this builds and runs its tests.
#   make test            system curl transport (the default)
#   make test-libcurl    the opt-in TRACE_CLIENT_USE_LIBCURL transport
CXX ?= g++
STD ?= c++11
CXXFLAGS ?= -std=$(STD) -Wall -Wextra -pedantic -Werror -pthread
# Where the tests find test/install-id-fixtures and the files whose version
# they compare; kept out of CXXFLAGS so overriding those keeps it.
SOURCE_DIR = -DTRACE_CLIENT_SOURCE_DIR=\"$(CURDIR)\"

.PHONY: test test-libcurl clean

build/test_trace_client: test/test_trace_client.cpp trace_client.hpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) $(SOURCE_DIR) test/test_trace_client.cpp -o $@

build/test_trace_client_libcurl: test/test_trace_client.cpp trace_client.hpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) $(SOURCE_DIR) -DTRACE_CLIENT_USE_LIBCURL test/test_trace_client.cpp -o $@ -lcurl

test: build/test_trace_client
	./build/test_trace_client

test-libcurl: build/test_trace_client_libcurl
	./build/test_trace_client_libcurl

clean:
	rm -rf build
