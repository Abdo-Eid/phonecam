"""Live, destructive-to-the-current-stream access-control probe (not a sniffing test).

Open PhoneCam first and stop phonecam-host. Uses PHONECAM_SECRET; never logs it.
Only stdlib, flatc on PATH, and the repository's vendored FlatBuffers are needed.
"""

import argparse
import hashlib
import hmac
import os
from pathlib import Path
import secrets
import socket
import struct
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[1]
GENERATED = ROOT / "windows" / "build" / "security-probe-python"
sys.path.insert(0, str(ROOT / "third_party" / "flatbuffers" / "python"))


def exact(sock, size):
    data = bytearray()
    while len(data) < size:
        part = sock.recv(size - len(data))
        if not part:
            raise EOFError("peer closed")
        data.extend(part)
    return bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host")
    args = parser.parse_args()
    code = os.environ.get("PHONECAM_SECRET", "").replace("-", "")
    if not code:
        parser.error("set PHONECAM_SECRET to the phone's pairing code")
    GENERATED.mkdir(parents=True, exist_ok=True)
    subprocess.run(["flatc", "--python", "-o", str(GENERATED),
                    str(ROOT / "proto" / "wire.fbs"), str(ROOT / "proto" / "control.fbs")], check=True)
    sys.path.insert(0, str(GENERATED))
    import flatbuffers
    from phonecam.control import (Ack, AuthChallenge, AuthResponse, ControlEnvelope,
                                  ControlPayload, StartVideo, StopVideo, StreamIntent)

    payload = ControlPayload.ControlPayload

    class Control:
        def __init__(self):
            self.sock = socket.create_connection((args.host, 27184), 5)
            self.sock.settimeout(8)
            self.command = 0
            self.stopped = None
            env = self.receive()
            assert env.PayloadType() == payload.AuthChallenge
            table = env.Payload()
            challenge = AuthChallenge.AuthChallenge()
            challenge.Init(table.Bytes, table.Pos)
            self.phone_nonce = challenge.Nonce().decode()
            self.pc_nonce = secrets.token_hex(16)
            proof = hmac.new(code.encode(), f"{self.phone_nonce}|{self.pc_nonce}".encode(), hashlib.sha256).hexdigest()
            builder = flatbuffers.Builder(256)
            nonce = builder.CreateString(self.pc_nonce)
            mac = builder.CreateString(proof)
            AuthResponse.Start(builder)
            AuthResponse.AddNonce(builder, nonce)
            AuthResponse.AddHmac(builder, mac)
            self.send(builder, payload.AuthResponse, AuthResponse.End(builder))
            while True:
                env = self.receive()
                if env.PayloadType() == payload.StreamIntent:
                    table = env.Payload()
                    intent = StreamIntent.StreamIntent()
                    intent.Init(table.Bytes, table.Pos)
                    self.stopped = intent.Stopped()
                if env.PayloadType() == payload.phonecam_wire_CapabilityDescriptor:
                    assert self.stopped is not None, "missing initial intent before capabilities"
                    break

        def receive(self):
            size, = struct.unpack("<I", exact(self.sock, 4))
            assert 0 < size <= 1024 * 1024
            return ControlEnvelope.ControlEnvelope.GetRootAs(exact(self.sock, size), 0)

        def send(self, builder, kind, value):
            self.command += 1
            ControlEnvelope.Start(builder)
            ControlEnvelope.AddCmdId(builder, self.command)
            ControlEnvelope.AddPayloadType(builder, kind)
            ControlEnvelope.AddPayload(builder, value)
            builder.Finish(ControlEnvelope.End(builder))
            data = bytes(builder.Output())
            self.sock.sendall(struct.pack("<I", len(data)) + data)

        def ack(self, expected=0):
            while True:
                env = self.receive()
                if env.PayloadType() == payload.Ack and env.CmdId() == self.command:
                    table = env.Payload()
                    response = Ack.Ack()
                    response.Init(table.Bytes, table.Pos)
                    assert response.Result() == expected, "unexpected session command result"
                    return

        def start(self, explicit=True, expected=0):
            builder = flatbuffers.Builder(128)
            StartVideo.Start(builder)
            StartVideo.AddMode(builder, 0)
            StartVideo.AddWidth(builder, 1280)
            StartVideo.AddHeight(builder, 720)
            StartVideo.AddFps(builder, 30)
            StartVideo.AddBitrateBps(builder, 4000000)
            StartVideo.AddExplicitStart(builder, explicit)
            self.send(builder, payload.StartVideo, StartVideo.End(builder))
            self.ack(expected)

        def stop(self, user_requested=False):
            builder = flatbuffers.Builder(64)
            StopVideo.Start(builder)
            StopVideo.AddUserRequested(builder, user_requested)
            self.send(builder, payload.StopVideo, StopVideo.End(builder))
            self.ack()

        def proof(self, nonce):
            message = f"phonecam-video-v1|{self.phone_nonce}|{self.pc_nonce}|{nonce}"
            return hmac.new(code.encode(), message.encode(), hashlib.sha256).hexdigest().encode()

    def video():
        deadline = time.monotonic() + 6
        while True:
            try:
                sock = socket.create_connection((args.host, 27183), 3)
                sock.settimeout(5)
                header = exact(sock, 20)
                assert header[:4] == b"PCV1", "missing mandatory video authentication"
                return sock, header[4:].hex()
            except (ConnectionRefusedError, TimeoutError):
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.2)

    def denied(sock):
        try:
            assert sock.recv(1) == b"", "unauthorized connection received extra bytes"
        except ConnectionResetError:
            pass
        finally:
            sock.close()

    control = Control()
    try:
        control.stop(user_requested=True)
        control.sock.close()
        control = Control()
        assert control.stopped is True, "Stop was lost across control reconnect"
        control.start(explicit=False, expected=3)
        try:
            unexpected = socket.create_connection((args.host, 27183), 1)
        except (ConnectionRefusedError, TimeoutError):
            pass
        else:
            unexpected.close()
            raise AssertionError("automatic Start opened video while stopped")
        print("PASS deliberate Stop survives reconnect and rejects automatic Start")
        control.start()
        sock, _ = video()
        denied(sock)
        print("PASS missing proof: closed without media; listener recovered")
        sock, _ = video()
        sock.sendall(b"0" * 64)
        denied(sock)
        print("PASS wrong proof: closed without media; listener recovered")
        sock, nonce = video()
        captured = control.proof(nonce)
        sock.sendall(captured)
        assert exact(sock, 1) == b"\x01"
        # Retire capture before it produces media; replay testing needs only the proof.
        control.stop()
        sock.close()
        control.start()
        sock, _ = video()
        sock.sendall(captured)
        denied(sock)
        print("PASS replay: old connection proof rejected")
        control.stop()
        old_control = control
        control.sock.close()
        time.sleep(1)
        control = Control()
        control.start()
        sock, nonce = video()
        sock.sendall(old_control.proof(nonce))
        denied(sock)
        print("PASS expired control session: old credentials rejected")
        sock, nonce = video()
        sock.sendall(control.proof(nonce))
        assert exact(sock, 1) == b"\x01"
        sock.settimeout(10)
        assert exact(sock, 20)[0] == 0x9C, "authorized client did not receive framed video"
        print("PASS current session: authorized client received video")
        control.sock.close()
        sock.settimeout(5)
        while sock.recv(65536):
            pass
        print("PASS control disconnect: video connection closed")
        sock.close()
    finally:
        control.sock.close()


if __name__ == "__main__":
    main()
