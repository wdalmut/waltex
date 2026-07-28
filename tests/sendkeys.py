#!/usr/bin/env python3
"""Inietta tasti in una VM QEMU attraverso il monitor su socket unix.

E' il modo di premere tasti senza avere una tastiera: senza questo, il driver
della tastiera si potrebbe verificare solo a mano.

Il monitor rimanda l'eco di ogni comando con le sequenze di escape del proprio
readline, quindi non proviamo a interpretare le risposte: mandiamo e basta.

    uso: sendkeys.py <percorso-socket> <tasto> [tasto...]
"""

import socket
import sys
import time


def main():
    if len(sys.argv) < 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2

    path = sys.argv[1]
    keys = sys.argv[2:]

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)

    # Il socket compare quando QEMU e' pronto, non quando lo lanciamo.
    for _ in range(50):
        try:
            sock.connect(path)
            break
        except OSError:
            time.sleep(0.1)
    else:
        print("sendkeys: monitor non raggiungibile su %s" % path,
              file=sys.stderr)
        return 1

    time.sleep(0.3)

    for key in keys:
        sock.sendall(("sendkey %s\n" % key).encode())
        # Un minimo di respiro: sendkey genera make e break code, e vogliamo
        # che il kernel li veda come pressioni distinte.
        time.sleep(0.05)

    time.sleep(0.3)
    sock.close()
    return 0


sys.exit(main())
