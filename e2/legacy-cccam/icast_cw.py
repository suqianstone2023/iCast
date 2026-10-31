#!/usr/bin/env python3
"""iCast dongle -> CCcam descrambler -> HTTP MPEG-TS for Enigma2.

Pipeline:
  icast-cli (full mux) -> single program -> ECM to CCcam -> CW
  -> CSA descramble (libdvbcsa) -> HTTP chunked stream

Everything runs in one process so the transport stream never leaves memory.
"""
import ctypes
import hashlib
import os
import queue
import socket
import struct
import subprocess
import sys
import threading
import time

TS = 188
CLI = "/home/root/icast/icast-cli"
ENCRYPT = 1
DECRYPT = 0

MSG_CLI_DATA = 0x00
MSG_CW_ECM = 0x01
MSG_KEEPALIVE = 0x06
MSG_CW_NOK1 = 0xfe
MSG_CW_NOK2 = 0xff

#
# CCcam endpoint comes from the environment so that no real credentials live
# in the repository.  Set ICAST_CCCAM_HOST / _PORT / _USER / _PASS before
# starting, or pass --host/--ccport/--user/--pass on the command line.
CFG = {
    "prog": 220, "pmt": 2200, "video": 2202, "audio": 2203,
    "ecmpid": 2205, "caid": 0x4A30, "cardid": 100,
    "port": 8110, "freq": "251", "sr": "6875", "qam": "64",
    "host": os.environ.get("ICAST_CCCAM_HOST", "127.0.0.1"),
    "ccport": int(os.environ.get("ICAST_CCCAM_PORT", "12000")),
    "user": os.environ.get("ICAST_CCCAM_USER", "user"),
    "pwd": os.environ.get("ICAST_CCCAM_PASS", "pass"),
}

i = 1
while i < len(sys.argv) - 1:
    k, v = sys.argv[i][2:], sys.argv[i + 1]
    if k in ("caid", "ecmpid", "prog", "pmt", "video", "audio", "port",
             "cardid", "ccport"):
        CFG[k] = int(v, 0)
    elif k in CFG:
        CFG[k] = v
    i += 2

PROG = CFG["prog"]
OUT_SET = set((0, CFG["pmt"], CFG["video"], CFG["audio"]))
BUF_MAX = 16 * 1024 * 1024


def log(msg):
    print("%s %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


# ---------------------------------------------------------------- CCcam crypto
class CcCrypt(object):
    def __init__(self, key):
        k = list(bytearray(key))
        n = len(k)
        self.kt = list(range(256))
        j = 0
        for i in range(256):
            j = (j + k[i % n] + self.kt[i]) & 0xff
            self.kt[i], self.kt[j] = self.kt[j], self.kt[i]
        self.state = k[0]
        self.counter = 0
        self.sum = 0

    def crypt(self, data, mode):
        out = bytearray(data)
        for i in range(len(out)):
            self.counter = (self.counter + 1) & 0xff
            self.sum = (self.sum + self.kt[self.counter]) & 0xff
            self.kt[self.counter], self.kt[self.sum] = \
                self.kt[self.sum], self.kt[self.counter]
            z = out[i]
            out[i] = z ^ self.kt[(self.kt[self.counter] + self.kt[self.sum]) & 0xff]
            out[i] ^= self.state
            if mode == DECRYPT:
                z = out[i]
            self.state ^= z
        return out


def cc_xor(d):
    d = bytearray(d)
    tag = b"CCcam\0"
    for i in range(8):
        d[8 + i] = (i * d[i]) & 0xff
        if i <= 5:
            d[i] ^= tag[i]
    return d


def cw_crypt(cws, node_id, cardid):
    u = int.from_bytes(node_id, "big")
    out = bytearray(cws)
    for i in range(16):
        tmp = out[i] ^ ((u >> (4 * i)) & 0xff)
        if i & 1:
            tmp = (~tmp) & 0xff
        out[i] = ((cardid >> (2 * i)) & 0xff) ^ tmp
    return out


class CCcam(object):
    NODE_ID = b"icast001"

    def __init__(self, timeout=6.0):
        self.timeout = timeout
        self.sock = None
        self.enc = None
        self.dec = None
        self.buf = b""

    def _recv(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise IOError("cccam connection closed")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def connect(self):
        self.buf = b""
        self.sock = socket.create_connection((CFG["host"], CFG["ccport"]),
                                             self.timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock.settimeout(self.timeout)
        seed = cc_xor(self._recv(16))
        h = bytearray(hashlib.sha1(bytes(seed)).digest())
        self.dec = CcCrypt(h)
        seed = self.dec.crypt(seed, DECRYPT)
        self.enc = CcCrypt(seed[:16])
        h = self.enc.crypt(h, DECRYPT)
        h = self.enc.crypt(h, ENCRYPT)
        self.sock.sendall(bytes(h))

        usr = bytearray(CFG["user"].encode()[:20].ljust(20, b"\0"))
        self.sock.sendall(bytes(self.enc.crypt(usr, ENCRYPT)))

        pwd = bytearray(CFG["pwd"].encode())
        self.enc.crypt(pwd, ENCRYPT)
        tag = bytearray(b"CCcam\0")
        self.sock.sendall(bytes(self.enc.crypt(tag, ENCRYPT)))

        resp = self.dec.crypt(self._recv(20), DECRYPT)
        if resp[:5] != b"CCcam":
            raise IOError("cccam login rejected")
        log("cccam login ok")

        cli = bytearray(93)
        cli[0:20] = CFG["user"].encode()[:20].ljust(20, b"\0")
        cli[20:28] = self.NODE_ID
        cli[29:34] = b"2.3.2"
        cli[61:65] = b"4000"
        self._send(MSG_CLI_DATA, bytes(cli))

    def _send(self, cmd, payload=b""):
        hdr = bytes([1, cmd, (len(payload) >> 8) & 0xff, len(payload) & 0xff])
        self.sock.sendall(bytes(self.enc.crypt(hdr + payload, ENCRYPT)))

    def recv_msg(self):
        hdr = self.dec.crypt(self._recv(4), DECRYPT)
        cmd = hdr[1]
        ln = (hdr[2] << 8) | hdr[3]
        payload = b""
        if ln:
            payload = self.dec.crypt(self._recv(ln), DECRYPT)
        if cmd == MSG_CW_ECM and len(payload) >= 16:
            plain = cw_crypt(payload[:16], self.NODE_ID, CFG["cardid"])
            self.dec.crypt(plain, ENCRYPT)   # NCam keeps the state in step here
            payload = bytes(plain) + bytes(payload[16:])
        return cmd, payload

    def request_cw(self, ecm):
        p = bytearray()
        p += struct.pack(">H", CFG["caid"])
        p += struct.pack(">I", 0)
        p += struct.pack(">I", CFG["cardid"])
        p += struct.pack(">H", PROG & 0xffff)
        p += bytes([len(ecm) & 0xff])
        p += ecm
        self._send(MSG_CW_ECM, bytes(p))
        end = time.time() + 3.0
        while time.time() < end:
            try:
                cmd, payload = self.recv_msg()
            except socket.timeout:
                continue
            if cmd == MSG_CW_ECM and len(payload) >= 16:
                return bytes(payload[:16])
            if cmd in (MSG_CW_NOK1, MSG_CW_NOK2):
                return None
            if cmd == MSG_KEEPALIVE:
                self._send(MSG_KEEPALIVE)
        return None

    def close(self):
        try:
            self.sock.close()
        except Exception:
            pass


# ------------------------------------------------------------------------ CSA
class Csa(object):
    def __init__(self):
        lib = ctypes.CDLL("libdvbcsa.so.1")
        lib.dvbcsa_key_alloc.restype = ctypes.c_void_p
        lib.dvbcsa_key_set.argtypes = [ctypes.c_char_p, ctypes.c_void_p]
        lib.dvbcsa_key_set.restype = None
        lib.dvbcsa_decrypt.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                       ctypes.c_uint]
        lib.dvbcsa_decrypt.restype = None
        self.lib = lib
        self.keys = {2: None, 3: None}
        self.cw = None

    def set_cw(self, cw16):
        for parity, half in ((2, cw16[0:8]), (3, cw16[8:16])):
            k = self.lib.dvbcsa_key_alloc()
            self.lib.dvbcsa_key_set(bytes(half), k)
            self.keys[parity] = k
        self.cw = bytes(cw16)
        log("cw installed %s" % self.cw.hex())

    def decrypt_inplace(self, pkt, off, length, parity):
        arr = (ctypes.c_uint8 * length).from_buffer(pkt, off)
        self.lib.dvbcsa_decrypt(self.keys[parity], arr, length)
        del arr


# ---------------------------------------------------------------- media bridge
class Bridge(object):
    def __init__(self):
        self.buf = bytearray()
        self.base = 0
        self.total = 0
        self.lock = threading.Lock()
        self.changed = threading.Condition(self.lock)
        self.csa = Csa()
        self.ecm_q = queue.Queue(maxsize=8)
        self.last_ecm = None
        self.stats = {"in": 0, "out": 0, "dec": 0, "ecm": 0, "nocw": 0}

    # ---- output ring ---------------------------------------------------
    def flush(self, out):
        if not out:
            return
        with self.changed:
            self.buf.extend(out)
            self.total += len(out)
            over = len(self.buf) - BUF_MAX
            if over > 0:
                over -= over % TS
                del self.buf[:over]
                self.base += over
            self.changed.notify_all()

    def follow(self, start):
        pos = start
        while True:
            with self.changed:
                if pos < self.base:
                    pos = self.base
                if pos >= self.total:
                    self.changed.wait(0.4)
                    continue
                off = pos - self.base
                data = bytes(self.buf[off:])
                pos = self.total
            yield data

    # ---- transport stream ----------------------------------------------
    def handle_ecm(self, buf, i):
        if not (buf[i + 1] & 0x40):
            return
        j = 4
        if (buf[i + 3] >> 4) & 2:
            j += 1 + buf[i + 4]
        if j >= TS:
            return
        p = i + j
        p += 1 + buf[p]
        end = i + TS
        if p + 3 > end:
            return
        ln = ((buf[p + 1] & 0x0f) << 8) | buf[p + 2]
        sec = bytes(buf[p:min(p + 3 + ln, end)])
        if len(sec) < 12:
            return
        self.stats["ecm"] += 1
        h = hashlib.md5(sec).digest()
        if h == self.last_ecm:
            return
        self.last_ecm = h
        try:
            self.ecm_q.put_nowait(sec)
        except queue.Full:
            pass

    def pump(self):
        ecmpid = CFG["ecmpid"]
        csa = self.csa
        while True:
            cmd = [CLI, "-R", "-C", "-f", CFG["freq"], "-r", CFG["sr"],
                   "-m", CFG["qam"], "-o", "-", "stream"]
            log("spawn %s" % " ".join(cmd))
            logf = open("/tmp/icast-cw-cli.log", "ab", 0)
            try:
                child = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                         stderr=logf, bufsize=0)
            except Exception as exc:
                log("spawn failed: %s" % exc)
                time.sleep(5)
                continue
            fd = child.stdout.fileno()
            buf = bytearray()
            while True:
                try:
                    chunk = os.read(fd, TS * 1500)
                except OSError:
                    chunk = b""
                if not chunk:
                    break
                self.stats["in"] += len(chunk)
                buf.extend(chunk)
                n = len(buf)
                i = 0
                out = bytearray()
                while i + TS <= n:
                    if buf[i] != 0x47:
                        i += 1
                        continue
                    pid = ((buf[i + 1] & 0x1f) << 8) | buf[i + 2]
                    if pid == ecmpid:
                        self.handle_ecm(buf, i)
                        i += TS
                        continue
                    if pid not in OUT_SET:
                        i += TS
                        continue
                    pkt = bytearray(buf[i:i + TS])
                    ctl = (pkt[3] >> 6) & 3
                    if ctl:
                        if csa.keys[ctl] is None:
                            self.stats["nocw"] += 1
                            i += TS
                            continue        # cannot descramble yet, drop
                        j = 4
                        if (pkt[3] >> 4) & 2:
                            j += 1 + pkt[4]
                        if j < TS:
                            ln = (TS - j) & ~7
                            if ln:
                                csa.decrypt_inplace(pkt, j, ln, ctl)
                        pkt[3] &= 0x3f
                        self.stats["dec"] += 1
                    out += pkt
                    i += TS
                if i:
                    del buf[:i]
                if out:
                    self.stats["out"] += len(out) // TS
                    self.flush(out)
            child.wait()
            log("dongle exited, restart in 3s")
            time.sleep(3)

    # ---- control words --------------------------------------------------
    def cw_worker(self):
        cc = None
        while True:
            ecm = self.ecm_q.get()
            while not self.ecm_q.empty():
                try:
                    ecm = self.ecm_q.get_nowait()
                except queue.Empty:
                    break
            if cc is None:
                try:
                    cc = CCcam()
                    cc.connect()
                except Exception as exc:
                    log("cccam connect failed: %s" % exc)
                    cc = None
                    time.sleep(2)
                    continue
            try:
                cw = cc.request_cw(ecm)
            except Exception as exc:
                log("cccam request failed: %s" % exc)
                cc.close()
                cc = None
                continue
            if cw:
                if cw != self.csa.cw:
                    self.csa.set_cw(cw)
            else:
                log("no cw for this ecm")

    def reporter(self):
        while True:
            time.sleep(15)
            d = self.stats
            log("stats in=%.1fMB out=%d dec=%d ecm=%d nocw=%d" %
                (d["in"] / 1e6, d["out"], d["dec"], d["ecm"], d["nocw"]))


def main():
    br = Bridge()
    threading.Thread(target=br.pump, daemon=True).start()
    threading.Thread(target=br.cw_worker, daemon=True).start()
    threading.Thread(target=br.reporter, daemon=True).start()

    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", CFG["port"]))
    srv.listen(8)
    log("listening on %d prog=%d pmt=%d es=%d,%d ecm=%d caid=%04X"
        % (CFG["port"], PROG, CFG["pmt"], CFG["video"], CFG["audio"],
           CFG["ecmpid"], CFG["caid"]))

    def handle(conn, peer):
        sent = 0
        try:
            conn.settimeout(10)
            req = conn.recv(2048)
            if not req:
                return
            log("REQ %s %s" % (peer, req.split(b"\r\n")[0].decode("latin-1")))
            conn.settimeout(120)
            conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\n"
                         b"Transfer-Encoding: chunked\r\n"
                         b"Cache-Control: no-cache\r\nConnection: close\r\n\r\n")
            with br.lock:
                start = br.total
            first = True
            for data in br.follow(start):
                if first:
                    k = data.find(b"\x47")
                    while k >= 0 and k + TS < len(data) and data[k + TS] != 0x47:
                        k = data.find(b"\x47", k + 1)
                    if k < 0:
                        continue
                    data = data[k:]
                    first = False
                conn.sendall(b"%x\r\n" % len(data) + data + b"\r\n")
                sent += len(data)
        except Exception as exc:
            log("client %s gone: %s" % (peer, exc))
        finally:
            log("CLOSE %s %d bytes" % (peer, sent))
            try:
                conn.close()
            except Exception:
                pass

    while True:
        conn, addr = srv.accept()
        threading.Thread(target=handle, args=(conn, addr[0]), daemon=True).start()


if __name__ == "__main__":
    main()
