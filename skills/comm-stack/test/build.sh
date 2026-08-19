#!/bin/sh
# 构建并运行 comm-stack 单元测试
# 依赖：gcc（MSYS2 ucrt64 或任意 C11 编译器），无需 make / cmake
set -e

cd "$(dirname "$0")"

OUT=comm_test

gcc -std=c11 -O1 -g \
    -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion \
    -Wpointer-arith -Wcast-align -Wstrict-prototypes \
    -I../src \
    ../src/comm_frame.c \
    ../src/comm_framer.c \
    ../src/comm_dispatch.c \
    ../src/comm_link.c \
    test_frame.c \
    test_framer.c \
    test_link.c \
    main.c \
    -o "$OUT"

echo "built ./$OUT"
./"$OUT"
