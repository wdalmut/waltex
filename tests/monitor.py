#!/usr/bin/env python3
"""Manda comandi arbitrari al monitor di QEMU su un socket unix.

sendkeys.py manda "sendkey", e basta a premere tasti. Da M10 serve anche
"quit", che non e' un tasto: chiudere QEMU dal monitor invece di ammazzarlo con
kill fa versare sul file le scritture ancora in volo, e tests/disk.sh rilegge
proprio quel file.

E' la seconda meta' della difesa contro un test intermittente. La prima e'
cache=writethrough nella riga di QEMU, la terza e' il comando FLUSH CACHE che
ata_dev_write manda al disco. Tre difese indipendenti contro lo stesso guasto,
che si manifesterebbe come un controllo verde nove volte su dieci — la peggiore
delle diagnosi.

    uso: monitor.py <percorso-socket> <comando> [comando...]
"""

import socket
import sys
import time


def main():
    if len(sys.argv) < 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2

    path = sys.argv[1]
    comandi = sys.argv[2:]

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)

    # Il socket compare quando QEMU e' pronto, non quando lo lanciamo.
    for _ in range(50):
        try:
            sock.connect(path)
            break
        except OSError:
            time.sleep(0.1)
    else:
        print("monitor: non raggiungibile su %s" % path, file=sys.stderr)
        return 1

    time.sleep(0.3)

    for c in comandi:
        sock.sendall((c + "\n").encode())
        time.sleep(0.1)

    time.sleep(0.3)
    sock.close()
    return 0


sys.exit(main())
