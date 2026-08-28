#!/bin/sh
# Builds and runs the verdict-logic tests on the host. No ESP-IDF needed.
set -e
cd "$(dirname "$0")"
cc -std=c11 -Wall -Wextra -Werror -O1 -g \
   -I../main \
   -o test_judge test_judge.c ../main/judge.c
./test_judge
