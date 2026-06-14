# PolyDraw C implementation — TDD build
CC      = cc
CFLAGS  = -std=c11 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter \
          -O2 -g -Isrc -Isrc/eval -Itests
LDFLAGS = -lm

SRC_EVAL = src/eval/pd_ir.c src/eval/pd_interp.c src/eval/pd_lexer.c \
           src/eval/pd_parser.c src/eval/pd_compile.c src/eval/pd_host.c \
           src/eval/pd_section.c
SRC_POLY = src/pd_polyhost.c

TESTS = build/test_interp build/test_lexer build/test_compile build/test_host build/test_section
TOOLS = build/polydraw-eval

all: $(TESTS) $(TOOLS)

build:
	mkdir -p build

build/test_interp: tests/test_interp.c src/eval/pd_ir.c src/eval/pd_interp.c \
                    src/eval/pd_ir.h src/eval/pd_interp.h tests/test_main.h | build
	$(CC) $(CFLAGS) -o $@ tests/test_interp.c src/eval/pd_ir.c src/eval/pd_interp.c $(LDFLAGS)

build/test_lexer: tests/test_lexer.c src/eval/pd_lexer.c src/eval/pd_ir.c \
                   src/eval/pd_lexer.h src/eval/pd_ir.h tests/test_main.h | build
	$(CC) $(CFLAGS) -o $@ tests/test_lexer.c src/eval/pd_lexer.c src/eval/pd_ir.c $(LDFLAGS)

build/test_compile: tests/test_compile.c $(SRC_EVAL) \
                    src/eval/pd_ir.h src/eval/pd_interp.h src/eval/pd_lexer.h \
                    src/eval/pd_parser.h src/eval/pd_compile.h src/eval/pd_host.h \
                    tests/test_main.h | build
	$(CC) $(CFLAGS) -o $@ tests/test_compile.c $(SRC_EVAL) $(LDFLAGS)

build/test_host: tests/test_host.c $(SRC_EVAL) \
                 src/eval/pd_compile.h src/eval/pd_host.h src/eval/pd_interp.h \
                 src/eval/pd_ir.h tests/test_main.h | build
	$(CC) $(CFLAGS) -o $@ tests/test_host.c $(SRC_EVAL) $(LDFLAGS)

build/test_section: tests/test_section.c src/eval/pd_section.c \
                     src/eval/pd_section.h tests/test_main.h | build
	$(CC) $(CFLAGS) -o $@ tests/test_section.c src/eval/pd_section.c $(LDFLAGS)

build/polydraw-eval: src/polydraw-eval.c $(SRC_EVAL) $(SRC_POLY) \
                     src/eval/pd_compile.h src/eval/pd_ir.h src/eval/pd_interp.h \
                     src/eval/pd_section.h src/pd_polyhost.h | build
	$(CC) $(CFLAGS) -o $@ src/polydraw-eval.c $(SRC_EVAL) $(SRC_POLY) $(LDFLAGS)

test: $(TESTS)
	@for t in $(TESTS); do echo "=== $$t ==="; $$t || exit 1; done

clean:
	rm -rf build

.PHONY: all test clean
