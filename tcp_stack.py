#!/usr/bin/python

import sys
import string
import socket
from time import sleep

data = string.digits + string.lowercase + string.uppercase


def server(port):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    s.bind(('0.0.0.0', int(port)))
    s.listen(3)

    cs, addr = s.accept()
    print addr

    fp = open('server-output.dat', 'w')

    while True:
        data = cs.recv(1000)
        # print(type(data))
        if data:
            # data = 'server echoes: ' + data
            # cs.send(data)
            fp.write(data)
        else:
            break
        sleep(0.01)

    fp.close()

    s.close()


def client(ip, port):
    s = socket.socket()
    s.connect((ip, int(port)))

    fp = open('client-input.dat', 'r')

    while True:
        data = fp.read(1024)
        if data:
            s.send(data)
        else:
            break
        sleep(0.05)

    fp.close()

    s.close()


if __name__ == '__main__':
    if sys.argv[1] == 'server':
        server(sys.argv[2])
    elif sys.argv[1] == 'client':
        client(sys.argv[2], sys.argv[3])
