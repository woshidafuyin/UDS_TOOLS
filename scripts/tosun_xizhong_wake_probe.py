#!/usr/bin/env python3
"""Standalone TOSUN TC1011 wake/probe for the Xizhong RSMR bench.

This script intentionally does not import the UDS tool or ProbeService.  It
calls libTSCAN directly and performs only the non-destructive entry check:

1. Configure TOSUN CH1 as ISO CAN FD, 500 kbit/s / 2 Mbit/s.
2. Send NM_ICG 0x18FFA025, extended Classic CAN, eight zero bytes, every 200 ms.
3. Treat source-address 0xB7 traffic as wake evidence.
4. Optionally send physical CAN FD+BRS 10 01 on 0x18DAB7F1 and wait for
   0x18DAF1B7 : 50 01.

No programming, security access, erase, download, reset, DTC control, or
communication-control service is sent.
"""

from __future__ import annotations

import argparse
import ctypes
import datetime as dt
import os
from pathlib import Path
import subprocess
import sys
import time
from collections import Counter
from typing import Iterable


SUCCESS = 0
ALREADY_CONNECTED = 5
CHANNEL_INDEX = 0  # TOSUN CH1 is zero-based in libTSCAN.
RX_ONLY = 1
ISO_CAN_FD = 1
NORMAL_MODE = 0

NM_ID = 0x18FFA025
PHYSICAL_TX_ID = 0x18DAB7F1
PHYSICAL_RX_ID = 0x18DAF1B7
ECU_SOURCE_ADDRESS = 0xB7

NM_PAYLOAD = bytes(8)
DEFAULT_SESSION_REQUEST = bytes((0x02, 0x10, 0x01, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC))


class TlibCan(ctypes.Structure):
    _fields_ = [
        ("channel", ctypes.c_uint8),
        ("properties", ctypes.c_uint8),
        ("dlc", ctypes.c_uint8),
        ("reserved", ctypes.c_uint8),
        ("identifier", ctypes.c_int32),
        ("time_us", ctypes.c_uint64),
        ("data", ctypes.c_uint8 * 8),
    ]


class TlibCanFd(ctypes.Structure):
    _fields_ = [
        ("channel", ctypes.c_uint8),
        ("properties", ctypes.c_uint8),
        ("dlc", ctypes.c_uint8),
        ("fd_properties", ctypes.c_uint8),
        ("identifier", ctypes.c_int32),
        ("time_us", ctypes.c_uint64),
        ("data", ctypes.c_uint8 * 64),
    ]


class Frame:
    def __init__(
        self,
        identifier: int,
        data: bytes,
        *,
        extended: bool,
        fd: bool,
        brs: bool,
    ) -> None:
        self.identifier = identifier
        self.data = data
        self.extended = extended
        self.fd = fd
        self.brs = brs


def dlc_to_length(dlc: int) -> int:
    lengths = (0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64)
    if not 0 <= dlc < len(lengths):
        raise ValueError(f"invalid CAN FD DLC: {dlc}")
    return lengths[dlc]


def make_classic_frame(identifier: int, payload: bytes) -> TlibCan:
    if len(payload) > 8:
        raise ValueError("Classic CAN payload exceeds 8 bytes")
    frame = TlibCan()
    frame.channel = CHANNEL_INDEX
    frame.properties = 0x01 | 0x04  # TX + extended identifier
    frame.dlc = len(payload)
    frame.identifier = identifier
    for index, value in enumerate(payload):
        frame.data[index] = value
    return frame


def make_fd_frame(identifier: int, payload: bytes, *, brs: bool) -> TlibCanFd:
    if len(payload) > 8:
        raise ValueError("This probe only permits CAN FD payloads up to 8 bytes")
    frame = TlibCanFd()
    frame.channel = CHANNEL_INDEX
    frame.properties = 0x01 | 0x04  # TX + extended identifier
    frame.dlc = len(payload)
    frame.fd_properties = 0x01 | (0x02 if brs else 0)  # EDL + optional BRS
    frame.identifier = identifier
    for index, value in enumerate(payload):
        frame.data[index] = value
    return frame


class AscWriter:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.started = time.monotonic()
        self.stream = path.open("w", encoding="ascii", newline="")
        now = dt.datetime.now().strftime("%a %b %d %I:%M:%S %p %Y")
        self.stream.write(
            f"date {now}\r\n"
            "base hex  timestamps absolute\r\n"
            "no internal events logged\r\n"
            f"Begin Triggerblock {now}\r\n"
        )
        self.stream.flush()

    def write(self, direction: str, frame: Frame) -> None:
        timestamp = time.monotonic() - self.started
        suffix = "x" if frame.extended else ""
        data = " ".join(f"{value:02X}" for value in frame.data)
        if frame.fd:
            line = (
                f"{timestamp:12.6f} CANFD 1 {direction} "
                f"{frame.identifier:x}{suffix} 1 {int(frame.brs)} "
                f"{len(frame.data):x} {len(frame.data)} {data} 0 0 0 0 0\r\n"
            )
        else:
            line = (
                f"{timestamp:12.6f} 1 {frame.identifier:x}{suffix} "
                f"{direction} d {len(frame.data)} {data}\r\n"
            )
        self.stream.write(line)
        self.stream.flush()

    def close(self) -> None:
        if self.stream.closed:
            return
        self.stream.write("End TriggerBlock\r\n")
        self.stream.close()

    def __enter__(self) -> "AscWriter":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()


class TscanError(RuntimeError):
    def __init__(self, operation: str, status: int, detail: str) -> None:
        super().__init__(f"{operation} failed: {detail} (status={status})")
        self.operation = operation
        self.status = status


class TscanApi:
    def __init__(self, dll_path: Path) -> None:
        self.dll_path = dll_path
        self._dll_directory = os.add_dll_directory(str(dll_path.parent))
        self.dll = ctypes.WinDLL(str(dll_path))
        self.handle: int | None = None
        self.initialized = False
        self._bind()

    def _bind(self) -> None:
        self.dll.initialize_lib_tscan.argtypes = [
            ctypes.c_bool,
            ctypes.c_bool,
            ctypes.c_bool,
        ]
        self.dll.initialize_lib_tscan.restype = None
        self.dll.finalize_lib_tscan.argtypes = []
        self.dll.finalize_lib_tscan.restype = None

        self.dll.tscan_scan_devices.argtypes = [ctypes.POINTER(ctypes.c_uint32)]
        self.dll.tscan_scan_devices.restype = ctypes.c_uint32
        self.dll.tscan_get_device_info.argtypes = [
            ctypes.c_int32,
            ctypes.POINTER(ctypes.c_char_p),
            ctypes.POINTER(ctypes.c_char_p),
            ctypes.POINTER(ctypes.c_char_p),
        ]
        self.dll.tscan_get_device_info.restype = ctypes.c_uint32
        self.dll.tscan_connect.argtypes = [
            ctypes.c_char_p,
            ctypes.POINTER(ctypes.c_size_t),
        ]
        self.dll.tscan_connect.restype = ctypes.c_uint32
        self.dll.tscan_get_can_channel_count.argtypes = [
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_int32),
        ]
        self.dll.tscan_get_can_channel_count.restype = ctypes.c_uint32
        self.dll.tscan_disconnect_by_handle.argtypes = [ctypes.c_size_t]
        self.dll.tscan_disconnect_by_handle.restype = ctypes.c_uint32
        self.dll.tscan_disconnect_all_devices.argtypes = []
        self.dll.tscan_disconnect_all_devices.restype = ctypes.c_uint32

        self.dll.tscan_config_canfd_by_baudrate.argtypes = [
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.c_double,
            ctypes.c_double,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_uint32,
        ]
        self.dll.tscan_config_canfd_by_baudrate.restype = ctypes.c_uint32
        self.dll.tscan_transmit_can_async.argtypes = [
            ctypes.c_size_t,
            ctypes.POINTER(TlibCan),
        ]
        self.dll.tscan_transmit_can_async.restype = ctypes.c_uint32
        self.dll.tscan_transmit_canfd_async.argtypes = [
            ctypes.c_size_t,
            ctypes.POINTER(TlibCanFd),
        ]
        self.dll.tscan_transmit_canfd_async.restype = ctypes.c_uint32
        self.dll.tsfifo_receive_canfd_msgs.argtypes = [
            ctypes.c_size_t,
            ctypes.POINTER(TlibCanFd),
            ctypes.POINTER(ctypes.c_int32),
            ctypes.c_uint8,
            ctypes.c_uint8,
        ]
        self.dll.tsfifo_receive_canfd_msgs.restype = ctypes.c_uint32
        describe = getattr(self.dll, "tscan_get_error_description", None)
        if describe is not None:
            describe.argtypes = [ctypes.c_uint32, ctypes.POINTER(ctypes.c_char_p)]
            describe.restype = ctypes.c_uint32

    @staticmethod
    def _decode(value: bytes | None) -> str:
        if not value:
            return ""
        for encoding in ("utf-8", "mbcs"):
            try:
                return value.decode(encoding)
            except UnicodeDecodeError:
                pass
        return value.decode("latin-1", errors="replace")

    def error_text(self, status: int) -> str:
        describe = getattr(self.dll, "tscan_get_error_description", None)
        if describe is None:
            return "unknown libTSCAN error"
        output = ctypes.c_char_p()
        result = describe(status, ctypes.byref(output))
        if result == SUCCESS and output.value:
            return self._decode(output.value)
        return "unknown libTSCAN error"

    def check(self, operation: str, status: int, accepted: Iterable[int] = (SUCCESS,)) -> None:
        if status not in accepted:
            raise TscanError(operation, status, self.error_text(status))

    def initialize(self) -> None:
        # Match the vendor demo and the validated C++ adapter.  Enabling the
        # hardware-time mode on this TC1011/HS CANFDMini caused the FD FIFO to
        # return no RX frames even though transmission and channel setup passed.
        self.dll.initialize_lib_tscan(True, False, False)
        self.initialized = True
        self.receive_stats: Counter[tuple[str, int, int]] = Counter()

    def enumerate_devices(self) -> list[tuple[str, str, str]]:
        count = ctypes.c_uint32()
        self.check("tscan_scan_devices", self.dll.tscan_scan_devices(ctypes.byref(count)))
        devices: list[tuple[str, str, str]] = []
        for index in range(count.value):
            manufacturer = ctypes.c_char_p()
            product = ctypes.c_char_p()
            serial = ctypes.c_char_p()
            self.check(
                f"tscan_get_device_info[{index}]",
                self.dll.tscan_get_device_info(
                    index,
                    ctypes.byref(manufacturer),
                    ctypes.byref(product),
                    ctypes.byref(serial),
                ),
            )
            devices.append(
                (
                    self._decode(manufacturer.value),
                    self._decode(product.value),
                    self._decode(serial.value),
                )
            )
        return devices

    def connect(self, serial: str) -> None:
        handle = ctypes.c_size_t()
        status = self.dll.tscan_connect(serial.encode("ascii"), ctypes.byref(handle))
        self.check("tscan_connect", status, (SUCCESS, ALREADY_CONNECTED))
        self.handle = handle.value
        channels = ctypes.c_int32()
        status = self.dll.tscan_get_can_channel_count(self.handle, ctypes.byref(channels))
        self.check("tscan_get_can_channel_count", status)
        if channels.value < 1:
            raise RuntimeError("connected TOSUN device has no CAN channel")

    def configure(self, termination: bool) -> None:
        if self.handle is None:
            raise RuntimeError("TOSUN device is not connected")
        status = self.dll.tscan_config_canfd_by_baudrate(
            self.handle,
            CHANNEL_INDEX,
            500.0,
            2000.0,
            ISO_CAN_FD,
            NORMAL_MODE,
            int(termination),
        )
        self.check("tscan_config_canfd_by_baudrate", status)

    def send_classic(self, identifier: int, payload: bytes) -> None:
        if self.handle is None:
            raise RuntimeError("TOSUN device is not connected")
        raw = make_classic_frame(identifier, payload)
        self.check(
            "tscan_transmit_can_async",
            self.dll.tscan_transmit_can_async(self.handle, ctypes.byref(raw)),
        )

    def send_fd(self, identifier: int, payload: bytes, *, brs: bool) -> None:
        if self.handle is None:
            raise RuntimeError("TOSUN device is not connected")
        raw = make_fd_frame(identifier, payload, brs=brs)
        self.check(
            "tscan_transmit_canfd_async",
            self.dll.tscan_transmit_canfd_async(self.handle, ctypes.byref(raw)),
        )

    def receive(self) -> Frame | None:
        if self.handle is None:
            raise RuntimeError("TOSUN device is not connected")
        raw = TlibCanFd()
        count = ctypes.c_int32(1)
        status = self.dll.tsfifo_receive_canfd_msgs(
            self.handle,
            ctypes.byref(raw),
            ctypes.byref(count),
            CHANNEL_INDEX,
            RX_ONLY,
        )
        self.receive_stats[("fd", status, count.value)] += 1
        if status == SUCCESS and count.value > 0:
            if raw.properties & 0x01:  # Ignore TX echo.
                return None
            fd = bool(raw.fd_properties & 0x01)
            brs = bool(raw.fd_properties & 0x02)
            length = dlc_to_length(raw.dlc) if fd else min(raw.dlc, 8)
            return Frame(
                raw.identifier & 0x1FFFFFFF,
                bytes(raw.data[:length]),
                extended=bool(raw.properties & 0x04),
                fd=fd,
                brs=brs,
            )
        return None

    def close(self) -> None:
        if self.handle is not None:
            try:
                self.dll.tscan_disconnect_by_handle(self.handle)
            except Exception:
                pass
            self.handle = None
        if self.initialized:
            try:
                self.dll.tscan_disconnect_all_devices()
                self.dll.finalize_lib_tscan()
            except Exception:
                pass
            self.initialized = False
        self._dll_directory.close()


def default_dll_path() -> Path:
    project = Path(__file__).resolve().parents[1]
    candidates = (
        project / "dist" / "drivers" / "tosun" / "libTSCAN.dll",
        project / "build" / "nmake-x64" / "Release" / "drivers" / "tosun" / "libTSCAN.dll",
        Path(r"C:\Program Files (x86)\TOSUN\TSMaster\bin64\libTSCAN.dll"),
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return candidates[0]


def default_trace_path() -> Path:
    project = Path(__file__).resolve().parents[1]
    timestamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    return project / "dist" / "logs" / f"standalone_tosun_xizhong_wake_{timestamp}.asc"


def running_conflicts() -> list[str]:
    try:
        completed = subprocess.run(
            ("tasklist", "/FO", "CSV", "/NH"),
            check=False,
            capture_output=True,
            text=True,
            encoding="mbcs",
            errors="replace",
        )
    except OSError:
        return []
    names = (
        "uds_tool.exe",
        "uds_tool_qt.exe",
        "tsmaster.exe",
        "canoe64.exe",
        "canalyzer64.exe",
    )
    lowered = completed.stdout.lower()
    return [name for name in names if f'"{name}"' in lowered]


def is_positive_default_session(frame: Frame) -> bool:
    if frame.identifier != PHYSICAL_RX_ID or len(frame.data) < 3:
        return False
    length = frame.data[0] & 0x0F
    return (
        frame.data[0] >> 4 == 0
        and length >= 2
        and frame.data[1] == 0x50
        and frame.data[2] == 0x01
    )


def frame_summary(frame: Frame) -> str:
    kind = "CANFD+BRS" if frame.fd and frame.brs else "CANFD" if frame.fd else "CAN"
    payload = " ".join(f"{value:02X}" for value in frame.data)
    return f"0x{frame.identifier:08X} {kind} [{len(frame.data)}] {payload}"


def drain_receive(
    api: TscanApi,
    trace: AscWriter,
    *,
    print_limit: list[int],
) -> list[Frame]:
    received: list[Frame] = []
    for _ in range(512):
        frame = api.receive()
        if frame is None:
            break
        received.append(frame)
        trace.write("Rx", frame)
        if print_limit[0] > 0:
            print(f"RX {frame_summary(frame)}")
            print_limit[0] -= 1
    return received


def run_interval(
    api: TscanApi,
    trace: AscWriter,
    duration_s: float,
    *,
    next_nm: list[float] | None,
    print_limit: list[int],
) -> list[Frame]:
    deadline = time.monotonic() + duration_s
    received: list[Frame] = []
    while time.monotonic() < deadline:
        now = time.monotonic()
        if next_nm is not None and now >= next_nm[0]:
            api.send_classic(NM_ID, NM_PAYLOAD)
            frame = Frame(NM_ID, NM_PAYLOAD, extended=True, fd=False, brs=False)
            trace.write("Tx", frame)
            print("TX NM  0x18FFA025 00 00 00 00 00 00 00 00")
            next_nm[0] += 0.200
            if next_nm[0] <= now:
                next_nm[0] = now + 0.200
        received.extend(drain_receive(api, trace, print_limit=print_limit))
        time.sleep(0.001)
    received.extend(drain_receive(api, trace, print_limit=print_limit))
    return received


def select_device(
    devices: list[tuple[str, str, str]], requested_serial: str | None
) -> tuple[str, str, str]:
    if not devices:
        raise RuntimeError("no TOSUN device found")
    if requested_serial:
        for device in devices:
            if device[2].casefold() == requested_serial.casefold():
                return device
        raise RuntimeError(f"TOSUN serial {requested_serial!r} was not found")
    if len(devices) > 1:
        details = ", ".join(f"{product}[{serial}]" for _, product, serial in devices)
        raise RuntimeError(f"multiple TOSUN devices found; use --serial: {details}")
    return devices[0]


def open_configured_api(args: argparse.Namespace) -> tuple[TscanApi, tuple[str, str, str]]:
    last_error: Exception | None = None
    for attempt in range(1, args.connect_attempts + 1):
        api = TscanApi(args.dll)
        try:
            api.initialize()
            devices = api.enumerate_devices()
            print(f"DEVICE_COUNT={len(devices)}")
            for manufacturer, product, serial in devices:
                print(f"DEVICE manufacturer={manufacturer} product={product} serial={serial}")
            device = select_device(devices, args.serial)
            api.connect(device[2])
            api.configure(args.termination)
            print(
                f"CHANNEL_CONFIG=PASS attempt={attempt}/{args.connect_attempts} "
                "CH1 ISO_CAN_FD 500k/2M"
            )
            return api, device
        except Exception as error:
            last_error = error
            print(
                f"CHANNEL_CONFIG=FAIL attempt={attempt}/{args.connect_attempts}: {error}",
                file=sys.stderr,
            )
            api.close()
            if attempt < args.connect_attempts:
                time.sleep(args.reconnect_delay_ms / 1000.0)
    assert last_error is not None
    raise last_error


def run_probe(args: argparse.Namespace) -> int:
    conflicts = running_conflicts()
    if conflicts:
        print("WARN_RUNNING_PROCESSES=" + ",".join(conflicts))
        if any(
            name in conflicts
            for name in ("uds_tool.exe", "uds_tool_qt.exe", "tsmaster.exe")
        ):
            print(
                "ERROR: close the UDS UI and TSMaster before the standalone TOSUN probe",
                file=sys.stderr,
            )
            return 3

    print(f"DLL={args.dll}")
    print(f"TRACE={args.trace}")
    print(
        "BASELINE=CH1,ISO_CAN_FD,500k/2M,"
        "NM 0x18FFA025 Classic EXT 00x8 @200ms,"
        "UDS 0x18DAB7F1 FD+BRS 02 10 01 CCx5"
    )

    api: TscanApi | None = None
    try:
        api, _ = open_configured_api(args)
        with AscWriter(args.trace) as trace:
            print_limit = [args.print_rx_limit]
            print(f"PRELISTEN_MS={args.prelisten_ms}")
            before = run_interval(
                api,
                trace,
                args.prelisten_ms / 1000.0,
                next_nm=None,
                print_limit=print_limit,
            )
            before_b7 = sum(
                1 for frame in before if (frame.identifier & 0xFF) == ECU_SOURCE_ADDRESS
            )

            print(f"NM_WARMUP_MS={args.nm_warmup_ms}")
            next_nm = [time.monotonic()]
            after = run_interval(
                api,
                trace,
                args.nm_warmup_ms / 1000.0,
                next_nm=next_nm,
                print_limit=print_limit,
            )
            after_b7 = sum(
                1 for frame in after if (frame.identifier & 0xFF) == ECU_SOURCE_ADDRESS
            )

            diagnostic_response: Frame | None = None
            if not args.wake_only:
                for attempt in range(1, args.uds_attempts + 1):
                    api.send_fd(PHYSICAL_TX_ID, DEFAULT_SESSION_REQUEST, brs=True)
                    request = Frame(
                        PHYSICAL_TX_ID,
                        DEFAULT_SESSION_REQUEST,
                        extended=True,
                        fd=True,
                        brs=True,
                    )
                    trace.write("Tx", request)
                    print(
                        f"TX UDS attempt={attempt}/{args.uds_attempts} "
                        "0x18DAB7F1 FD+BRS 02 10 01 CC CC CC CC CC"
                    )
                    responses = run_interval(
                        api,
                        trace,
                        args.uds_timeout_ms / 1000.0,
                        next_nm=next_nm,
                        print_limit=print_limit,
                    )
                    after.extend(responses)
                    after_b7 += sum(
                        1
                        for frame in responses
                        if (frame.identifier & 0xFF) == ECU_SOURCE_ADDRESS
                    )
                    diagnostic_response = next(
                        (frame for frame in responses if is_positive_default_session(frame)),
                        None,
                    )
                    if diagnostic_response is not None:
                        break

            tail = run_interval(
                api,
                trace,
                args.postlisten_ms / 1000.0,
                next_nm=next_nm,
                print_limit=print_limit,
            )
            after.extend(tail)
            after_b7 += sum(
                1 for frame in tail if (frame.identifier & 0xFF) == ECU_SOURCE_ADDRESS
            )

        wake_pass = before_b7 > 0 or after_b7 > 0
        transition = before_b7 == 0 and after_b7 > 0
        print(f"PRE_NM_B7_RX={before_b7}")
        print(f"POST_NM_B7_RX={after_b7}")
        print(
            "WAKE_VERDICT="
            + (
                "PASS_NM_WAKE_TRANSITION"
                if transition
                else "PASS_ALREADY_AWAKE"
                if wake_pass
                else "FAIL_NO_XIZHONG_B7_TRAFFIC"
            )
        )
        if args.wake_only:
            print("UDS_VERDICT=SKIPPED")
        elif diagnostic_response is not None:
            print("UDS_VERDICT=PASS_50_01")
            print("UDS_RESPONSE=" + frame_summary(diagnostic_response))
        else:
            print("UDS_VERDICT=FAIL_NO_50_01")
        stats = ",".join(
            f"{fifo}:status={status}:count={count}:calls={calls}"
            for (fifo, status, count), calls in sorted(api.receive_stats.items())
        )
        print("FIFO_STATS=" + stats)
        return 0 if wake_pass else 2
    except KeyboardInterrupt:
        print("RESULT=CANCELLED", file=sys.stderr)
        return 130
    except Exception as error:
        print(f"RESULT=HARDWARE_ERROR: {error}", file=sys.stderr)
        return 3
    finally:
        if api is not None:
            api.close()


def self_test() -> int:
    assert ctypes.sizeof(TlibCan) == 24
    assert ctypes.sizeof(TlibCanFd) == 80
    classic = make_classic_frame(NM_ID, NM_PAYLOAD)
    assert classic.channel == 0
    assert classic.properties == 0x05
    assert classic.dlc == 8
    assert classic.identifier == NM_ID
    fd = make_fd_frame(PHYSICAL_TX_ID, DEFAULT_SESSION_REQUEST, brs=True)
    assert fd.channel == 0
    assert fd.properties == 0x05
    assert fd.fd_properties == 0x03
    assert fd.dlc == 8
    assert bytes(fd.data[:8]) == DEFAULT_SESSION_REQUEST
    positive = Frame(
        PHYSICAL_RX_ID,
        bytes((0x06, 0x50, 0x01, 0x00, 0x32, 0x00, 0xC8, 0x00)),
        extended=True,
        fd=False,
        brs=False,
    )
    assert is_positive_default_session(positive)
    print("SELF_TEST=PASS")
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Standalone TOSUN TC1011 wake/10 01 probe for Xizhong RSMR"
    )
    parser.add_argument("--dll", type=Path, default=default_dll_path())
    parser.add_argument("--trace", type=Path, default=default_trace_path())
    parser.add_argument("--serial")
    parser.add_argument("--termination", action="store_true")
    parser.add_argument("--prelisten-ms", type=int, default=500)
    parser.add_argument("--nm-warmup-ms", type=int, default=2000)
    parser.add_argument("--postlisten-ms", type=int, default=300)
    parser.add_argument("--uds-attempts", type=int, default=3)
    parser.add_argument("--uds-timeout-ms", type=int, default=1200)
    parser.add_argument("--connect-attempts", type=int, default=3)
    parser.add_argument("--reconnect-delay-ms", type=int, default=1200)
    parser.add_argument("--print-rx-limit", type=int, default=80)
    parser.add_argument("--wake-only", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    for name in (
        "prelisten_ms",
        "nm_warmup_ms",
        "postlisten_ms",
        "uds_attempts",
        "uds_timeout_ms",
        "connect_attempts",
        "reconnect_delay_ms",
        "print_rx_limit",
    ):
        if getattr(args, name) < 0:
            parser.error(f"--{name.replace('_', '-')} must be non-negative")
    if args.uds_attempts == 0 and not args.wake_only:
        parser.error("--uds-attempts must be positive unless --wake-only is used")
    if args.connect_attempts == 0:
        parser.error("--connect-attempts must be positive")
    return args


def main() -> int:
    args = parse_args()
    if args.self_test:
        return self_test()
    if not args.dll.is_file():
        print(f"RESULT=DLL_NOT_FOUND: {args.dll}", file=sys.stderr)
        return 3
    return run_probe(args)


if __name__ == "__main__":
    raise SystemExit(main())
