CC = clang -target x86_64-linux-gnu
AR = llvm-ar
CFLAGS = -std=c99 -O2 -Wall -Wextra -Werror -Wpedantic -ffreestanding -fno-builtin -fno-stack-protector
CPPFLAGS = -Iinclude

all: libncc.a ncc nas nld

build:
	mkdir -p build

build/runtime.o: lib/runtime.c include/ncc.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/platform.o: platform/linux_x86_64.c include/ncc.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

libncc.a: build/runtime.o build/platform.o build/syscall.o
	$(AR) rcs $@ $^

build/syscall.o: platform/linux_syscall.S | build
	nasm -f elf64 $< -o $@

clean:
	rm -rf build libncc.a ncc nas nld

.PHONY: all clean

build/common.o: tool/common.c include/tool.h include/ncc.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/assemble.o: tool/assemble.c include/tool.h include/ncc.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/link.o: tool/link.c include/tool.h include/ncc.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/driver.o: tool/driver.c include/tool.h include/ncc.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
tool/syscall.o: tool/syscall.S
	nasm -f elf64 $< -o $@
tool/start.o: tool/start.nas
	nasm -f elf64 $< -o $@
nas: tool/start.o tool/syscall.o build/driver.o build/common.o build/assemble.o build/link.o build/lex.o build/preprocess.o build/compile.o libncc.a
	ld.lld -nostdlib -static -no-pie $^ -o $@

nld: nas
	cp nas nld

ncc: nas
	cp nas ncc

build/lex.o: tool/lex.c include/front.h include/tool.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/preprocess.o: tool/preprocess.c include/front.h include/tool.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/compile.o: tool/compile.c include/front.h include/tool.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(addprefix build/,runtime.o platform.o common.o assemble.o link.o driver.o lex.o preprocess.o compile.o): $(wildcard include/*.h)
