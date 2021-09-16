#!/usr/bin/env python3

with open('test.txt', 'r') as f:
    last = 0
    cnt = 1
    for line in f.readlines():
        a, b = line.split()
        a = int(a)
        b = int(b)
        if last != 0:
            if a != last:
                print(cnt)
            last = a + b
        cnt += 1
