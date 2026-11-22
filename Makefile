CC ?= gcc
CFLAGS := -std=c17 -Wall -Wextra -Werror -O2

ifeq ($(OS),Windows_NT)
  EXE := .exe
else
  EXE :=
endif

SRCS := src/main.c src/lexer.c src/parser.c src/checker.c src/emitter.c
TARGET := fc2c$(EXE)
RUNNER := tests/test_runner$(EXE)

.PHONY: all sample test clean

all: $(TARGET)

$(TARGET): $(SRCS) src/fc.h
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET)

sample: $(TARGET)
	./$(TARGET) examples/sum_squares.fc --out generated/sum_squares.c
	$(CC) $(CFLAGS) generated/sum_squares.c -o generated/sum_squares$(EXE)
	./generated/sum_squares$(EXE)

$(RUNNER): tests/test_runner.c
	$(CC) $(CFLAGS) tests/test_runner.c -o $(RUNNER)

test: $(TARGET) $(RUNNER)
	./$(RUNNER)

clean:
	rm -f $(TARGET) $(RUNNER) generated/sum_squares$(EXE)
	rm -rf tests/work
