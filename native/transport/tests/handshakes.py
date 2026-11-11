"""Real gRPC/Boost TLS handshakes against an independent ephemeral signer."""
import argparse
import datetime
import json
import os
import pathlib
import select
import socket
import ssl
import struct
import subprocess
import tempfile
import threading
import time

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID


def exact(connection, size):
    data = b""
    while len(data) < size:
        part = connection.recv(size - len(data))
        if not part:
            raise EOFError
        data += part
    return data


class Signer:
    """Test backend implements the documented wire contract independently."""

    def __init__(self, path, key):
        self.path, self.key = str(path), key
        self.wrong_key = ec.generate_private_key(ec.SECP384R1())
        self.mode = "allow"
        self.serial = False
        self.accepted = []
        # Seconds from a stalled or slow request to the client closing it.
        self.abandoned_after = None
        self.listener = socket.socket(socket.AF_UNIX)
        self.listener.bind(self.path)
        # A small backlog, as a single-threaded production signer may use.
        self.listener.listen(2)
        threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        while True:
            try:
                conn, _ = self.listener.accept()
            except OSError:
                return
            if self.serial:
                self.handle(conn)
            else:
                threading.Thread(target=self.handle, args=(conn,), daemon=True).start()

    def handle(self, conn):
        with conn:
            conn.settimeout(2)
            try:
                size = struct.unpack("!H", exact(conn, 2))[0]
                if size > 254:
                    raise ValueError("oversize")
                data = exact(conn, size)
                if len(data) < 9 or data[:7] != b"LCS\x01\x01\x05\x03":
                    raise ValueError("untyped operation")
                hash_id, id_size = data[7:9]
                digest_size = {1: 32, 2: 48}.get(hash_id)
                if digest_size is None or data[9:9 + id_size] != b"identity":
                    raise ValueError("hash or identity")
                digest = data[9 + id_size:]
                if len(digest) != digest_size:
                    raise ValueError("transcript size")
                mode = self.mode
                received = time.monotonic()
                if mode == "stall":
                    # Hold the request until the client abandons it at its
                    # signer deadline.
                    conn.settimeout(8)
                    try:
                        if conn.recv(1) == b"":
                            self.abandoned_after = time.monotonic() - received
                    except socket.timeout:
                        pass
                    return
                if mode == "disconnect":
                    return
                if mode == "deny":
                    conn.sendall(b"\x00\x05LCS\x01\x01")
                    return
                if mode == "oversize":
                    conn.sendall(b"\xff\xff")
                    return
                if mode == "truncated":
                    conn.sendall(b"\x00\x0aLCS")
                    return
                if mode == "malformed":
                    conn.sendall(b"\x00\x0dLCS\x01\x00" + b"\x00" * 8)
                    return
                # Independently reconstruct the RFC 8446 server CV message.
                message = b" " * 64 + b"TLS 1.3, server CertificateVerify\0" + digest
                signing_key = self.wrong_key if mode == "wrong-key" else self.key
                signature = signing_key.sign(message, ec.ECDSA(hashes.SHA384()))
                payload = b"LCS\x01\x00" + signature
                wire = struct.pack("!H", len(payload)) + payload
                if mode == "delay":
                    time.sleep(0.1)
                    conn.sendall(wire)
                    return
                if mode == "slow":
                    try:
                        for byte in wire:
                            conn.sendall(bytes([byte]))
                            time.sleep(0.08)
                    except OSError:
                        self.abandoned_after = time.monotonic() - received
                    return
                self.accepted.append(digest_size)
                conn.sendall(wire[:1])
                conn.sendall(wire[1:4])
                conn.sendall(wire[4:])
            except (ValueError, EOFError, OSError):
                try:
                    conn.sendall(b"\x00\x05LCS\x01\x01")
                except OSError:
                    pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--provider", required=True)
    parser.add_argument("--runner", nargs=argparse.REMAINDER, default=[])
    args = parser.parse_args()
    now = datetime.datetime.now(datetime.timezone.utc)
    ca = ec.generate_private_key(ec.SECP384R1())
    server_key = ec.generate_private_key(ec.SECP384R1())
    client_key = ec.generate_private_key(ec.SECP384R1())
    other_key = ec.generate_private_key(ec.SECP384R1())
    name = lambda value: x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, value)])

    def certificate(key, subject, *, is_ca=False, is_server=False, issuer_key=None):
        builder = (x509.CertificateBuilder().subject_name(name(subject))
                   .issuer_name(name("Ephemeral transport CA"))
                   .public_key(key.public_key()).serial_number(x509.random_serial_number())
                   .not_valid_before(now - datetime.timedelta(days=1))
                   .not_valid_after(now + datetime.timedelta(days=1))
                   .add_extension(x509.BasicConstraints(ca=is_ca, path_length=None), True))
        if is_server:
            builder = builder.add_extension(
                x509.SubjectAlternativeName([x509.DNSName("transport.test")]), False)
        if not is_ca:
            usage = ExtendedKeyUsageOID.SERVER_AUTH if is_server else ExtendedKeyUsageOID.CLIENT_AUTH
            builder = builder.add_extension(x509.ExtendedKeyUsage([usage]), False)
        return builder.sign(issuer_key or ca, hashes.SHA384()).public_bytes(serialization.Encoding.PEM)

    descriptors = []

    def memory_file(label, data):
        fd = os.memfd_create(label)
        os.write(fd, data)
        descriptors.append(fd)
        return f"/proc/self/fd/{fd}"

    def private_pem(key):
        return key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                 serialization.NoEncryption())

    try:
        with tempfile.TemporaryDirectory(prefix="linuxcnc-transport-") as temp:
            folder = pathlib.Path(temp)
            root = folder / "ca.crt"
            root.write_bytes(certificate(ca, "Ephemeral transport CA", is_ca=True))
            cert = folder / "server.crt"
            cert.write_bytes(certificate(server_key, "transport.test", is_server=True))
            point = folder / "public.point"
            point.write_bytes(server_key.public_key().public_bytes(
                serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint))
            wrong_point = folder / "wrong.point"
            wrong_point.write_bytes(other_key.public_key().public_bytes(
                serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint))
            client_cert = memory_file("transport-test-client-certificate", certificate(client_key, "client"))
            bad_cert = memory_file("transport-test-untrusted-certificate",
                                   certificate(client_key, "client", issuer_key=other_key))
            client_private = memory_file("transport-ephemeral-client-key", private_pem(client_key))
            server_private = memory_file("transport-ephemeral-file-key", private_pem(server_key))
            signer = Signer(folder / "signer.sock", server_key)

            def command(mode, cipher="TLS_AES_256_GCM_SHA384", public_point=point):
                source = server_private if mode.endswith("-file") else str(public_point)
                return args.runner + [args.server, mode, args.provider, str(cert), str(root),
                                      source, signer.path, cipher]

            probe = subprocess.run(command("probe"), capture_output=True, text=True,
                                   pass_fds=descriptors, timeout=15)
            assert probe.returncode == 0, probe.stdout + probe.stderr
            assert sorted(signer.accepted) == [32, 48], signer.accepted
            print(probe.stdout.strip())
            mismatch = subprocess.run(command("probe", public_point=wrong_point),
                                      capture_output=True, text=True, timeout=10)
            assert mismatch.returncode != 0, "certificate/key mismatch accepted"
            for public_point in (point, wrong_point):
                release = subprocess.run(command("release", public_point=public_point),
                                         capture_output=True, text=True, timeout=10)
                assert release.returncode == 0, release.stdout + release.stderr
            for raw in (b"\xa5" * 48, b"\x00" * 146,
                        b"LCS\x01\x02\x05\x03\x02\x08identity" + b"\xa5" * 48):
                with socket.socket(socket.AF_UNIX) as connection:
                    connection.connect(signer.path)
                    connection.sendall(struct.pack("!H", len(raw)) + raw)
                    assert exact(connection, 7) == b"\x00\x05LCS\x01\x01"

            results = []
            modes = [("grpc", "TLS_AES_256_GCM_SHA384"),
                     ("wss", "TLS_AES_256_GCM_SHA384"),
                     ("wss", "TLS_AES_128_GCM_SHA256"),
                     ("grpc-file", "TLS_AES_256_GCM_SHA384"),
                     ("wss-file", "TLS_AES_256_GCM_SHA384")]
            for mode, cipher in modes:
                print(f"checking {mode} {cipher}", flush=True)
                signer.mode = "allow"
                with tempfile.TemporaryFile(mode="w+") as log:
                    process = subprocess.Popen(command(mode, cipher), stdout=subprocess.PIPE,
                                               stderr=log, text=True, pass_fds=descriptors)
                    try:
                        ready, _, _ = select.select([process.stdout], [], [], 15)
                        line = process.stdout.readline() if ready else ""
                        if not line.startswith("READY "):
                            log.seek(0)
                            raise AssertionError(line + log.read())
                        port = int(line.split()[1])
                        saved = {}

                        def connect(*, client=True, untrusted=False, hostname="transport.test",
                                    version=ssl.TLSVersion.TLSv1_3, resume=False, retain=False,
                                    timeout=2):
                            context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
                            context.minimum_version = context.maximum_version = version
                            context.load_verify_locations(root)
                            context.set_alpn_protocols(["h2"])
                            if client:
                                context.load_cert_chain(bad_cert if untrusted else client_cert,
                                                        client_private)
                            if resume:
                                context = saved["context"]
                            with socket.create_connection(("127.0.0.1", port), timeout=timeout) as raw:
                                with context.wrap_socket(raw, server_hostname=hostname,
                                                         session=saved["session"] if resume else None) as conn:
                                    if mode.startswith("wss"):
                                        conn.sendall(b"GET /transport HTTP/1.1\r\nHost: transport.test\r\n"
                                                     b"Upgrade: websocket\r\nConnection: Upgrade\r\n"
                                                     b"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                                     b"Sec-WebSocket-Version: 13\r\n\r\n")
                                        if not conn.recv(2048).startswith(b"HTTP/1.1 101"):
                                            raise ConnectionError("no authenticated WebSocket upgrade")
                                    else:
                                        conn.sendall(b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
                                                     b"\x00\x00\x00\x04\x00\x00\x00\x00\x00")
                                        if len(conn.recv(1024)) < 9:
                                            raise ConnectionError("no authenticated HTTP/2 settings")
                                        assert conn.selected_alpn_protocol() == "h2"
                                    assert conn.version() == "TLSv1.3"
                                    assert not conn.session_reused, "session authentication bypass"
                                    assert not conn.session.has_ticket, "server issued a session ticket"
                                    if retain:
                                        saved.update(context=context, session=conn.session)

                        def rejected(**kwargs):
                            try:
                                connect(**kwargs)
                            except OSError:
                                return
                            raise AssertionError(f"unexpected connection success: {mode} {kwargs}")

                        before = len(signer.accepted)
                        connect(retain=True)
                        connect(resume=True)
                        if mode.endswith("-file"):
                            assert len(signer.accepted) == before, "file key invoked external signer"
                        else:
                            assert len(signer.accepted) >= before + 2, "fresh authentication missing"
                        rejected(client=False)
                        rejected(untrusted=True)
                        rejected(hostname="wrong.test")
                        rejected(version=ssl.TLSVersion.TLSv1_2)
                        if not mode.endswith("-file"):
                            for fault in ("deny", "disconnect", "truncated", "oversize",
                                          "malformed", "wrong-key"):
                                signer.mode = fault
                                start = time.monotonic()
                                rejected()
                                assert time.monotonic() - start < 2.5, f"unbounded {fault}"
                            # The client waits longer than the signer deadline, so
                            # only the transport's own deadline can end these.
                            for fault in ("stall", "slow"):
                                signer.mode = fault
                                signer.abandoned_after = None
                                rejected(timeout=8)
                                deadline = time.monotonic() + 2
                                while signer.abandoned_after is None and time.monotonic() < deadline:
                                    time.sleep(0.01)
                                assert signer.abandoned_after is not None, f"{fault} request never abandoned"
                                assert signer.abandoned_after < 4.5, (
                                    f"{fault} held {signer.abandoned_after:.2f}s past the signer deadline")
                            if mode == "grpc":
                                # A client opening several connections at once
                                # waits its turn at a serial signer whose
                                # backlog is smaller than the burst.
                                signer.mode = "delay"
                                signer.serial = True
                                failures = []

                                def concurrent_connect():
                                    try:
                                        connect()
                                    except Exception as error:
                                        failures.append(error)

                                attempts = [threading.Thread(target=concurrent_connect)
                                            for _ in range(8)]
                                for attempt in attempts:
                                    attempt.start()
                                for attempt in attempts:
                                    attempt.join()
                                signer.serial = False
                                assert not failures, f"concurrent handshakes failed: {failures}"
                            signer.mode = "deny"
                            rejected(resume=True)
                            signer.mode = "allow"
                            connect()
                            detached = signer.path + ".away"
                            os.rename(signer.path, detached)
                            try:
                                rejected()
                            finally:
                                os.rename(detached, signer.path)
                        results.append({"transport": mode, "cipher": cipher,
                                        "tickets": False, "verified": True})
                    except BaseException:
                        log.seek(0)
                        print(log.read(), flush=True)
                        raise
                    finally:
                        process.terminate()
                        try:
                            process.wait(timeout=3)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait(timeout=3)
            assert {32, 48}.issubset(set(signer.accepted))
            signer.listener.close()
            print(json.dumps(results, indent=2))
    finally:
        for fd in descriptors:
            os.close(fd)


if __name__ == "__main__":
    main()
