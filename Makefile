# Simple Makefile for redis-from-scratch

CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2

TARGET_SERVER := server
TARGET_CLIENT := client

SRC_DIR := src
UTILS := $(SRC_DIR)/utils/utils.cpp
ASYNC := $(SRC_DIR)/multithreading/asyncio.cpp

SERVER_SRC := $(SRC_DIR)/server.cpp
CLIENT_SRC := $(SRC_DIR)/client.cpp

.PHONY: all clean

all: $(TARGET_SERVER) $(TARGET_CLIENT)

$(TARGET_SERVER): $(SERVER_SRC) $(ASYNC) $(UTILS)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(TARGET_CLIENT): $(CLIENT_SRC) $(UTILS)
	$(CXX) $(CXXFLAGS) $^ -o $@

clean:
	rm -f $(TARGET_SERVER) $(TARGET_CLIENT)
