#!/usr/bin/env python3
import argparse
import ctypes
import datetime as dt
import pathlib
import time


XL_SUCCESS = 0
XL_BUS_TYPE_CAN = 1
XL_INTERFACE_VERSION_V4 = 4
XL_ACTIVATE_RESET_CLOCK = 8
XL_CAN_EV_TAG_RX_OK = 0x0400
XL_CAN_EV_TAG_TX_MSG = 0x0440


class XlCanFdConf(ctypes.Structure):
    _pack_ = 8
    _fields_ = [
        ("arbitration_bitrate", ctypes.c_uint32),
        ("sjw_arbitration", ctypes.c_uint32),
        ("tseg1_arbitration", ctypes.c_uint32),
        ("tseg2_arbitration", ctypes.c_uint32),
        ("data_bitrate", ctypes.c_uint32),
        ("sjw_data", ctypes.c_uint32),
        ("tseg1_data", ctypes.c_uint32),
        ("tseg2_data", ctypes.c_uint32),
        ("reserved", ctypes.c_uint8),
        ("options", ctypes.c_uint8),
        ("reserved1", ctypes.c_uint8 * 2),
        ("reserved2", ctypes.c_uint32),
    ]


class XlCanRxMsg(ctypes.Structure):
    _fields_ = [
        ("can_id", ctypes.c_uint32),
        ("msg_flags", ctypes.c_uint32),
        ("crc", ctypes.c_uint32),
        ("reserved1", ctypes.c_uint8 * 12),
        ("total_bit_count", ctypes.c_uint16),
        ("dlc", ctypes.c_uint8),
        ("reserved", ctypes.c_uint8 * 5),
        ("data", ctypes.c_uint8 * 64),
    ]


class XlCanRxData(ctypes.Union):
    _fields_ = [("can_rx_ok", XlCanRxMsg), ("raw", ctypes.c_uint8 * 96)]


class XlCanRxEvent(ctypes.Structure):
    _fields_ = [
        ("size", ctypes.c_int32),
        ("tag", ctypes.c_uint16),
        ("channel_index", ctypes.c_uint8),
        ("reserved", ctypes.c_uint8),
        ("user_handle", ctypes.c_int32),
        ("chip_flags", ctypes.c_uint16),
        ("reserved0", ctypes.c_uint16),
        ("reserved1", ctypes.c_uint64),
        ("timestamp", ctypes.c_uint64),
        ("tag_data", XlCanRxData),
    ]


class XlCanTxMsg(ctypes.Structure):
    _fields_ = [
        ("can_id", ctypes.c_uint32),
        ("msg_flags", ctypes.c_uint32),
        ("dlc", ctypes.c_uint8),
        ("reserved", ctypes.c_uint8 * 7),
        ("data", ctypes.c_uint8 * 64),
    ]


class XlCanTxData(ctypes.Union):
    _fields_ = [("can_msg", XlCanTxMsg), ("raw", ctypes.c_uint8 * 80)]


class XlCanTxEvent(ctypes.Structure):
    _fields_ = [
        ("tag", ctypes.c_uint16),
        ("transaction_id", ctypes.c_uint16),
        ("channel_index", ctypes.c_uint8),
        ("reserved", ctypes.c_uint8 * 3),
        ("tag_data", XlCanTxData),
    ]


def dlc_length(dlc):
    return (0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64)[min(dlc, 15)]


def format_data(data):
    return " ".join(f"{value:02X}" for value in data)


class VectorProbe:
    def __init__(self, channel, asc_path, verbose_rx=False):
        self.channel = channel
        self.mask = 1 << (channel - 1)
        self.port = ctypes.c_long(-1)
        self.dll = ctypes.WinDLL("vxlapi64.dll")
        self.started = time.perf_counter()
        self.verbose_rx = verbose_rx
        self.asc = asc_path.open("w", encoding="ascii", newline="\n")
        self._configure_api()

    def _configure_api(self):
        self.dll.xlOpenDriver.restype = ctypes.c_int
        self.dll.xlOpenPort.argtypes = [
            ctypes.POINTER(ctypes.c_long), ctypes.c_char_p, ctypes.c_uint64,
            ctypes.POINTER(ctypes.c_uint64), ctypes.c_uint, ctypes.c_uint,
            ctypes.c_uint,
        ]
        self.dll.xlOpenPort.restype = ctypes.c_int
        self.dll.xlCanFdSetConfiguration.argtypes = [ctypes.c_long, ctypes.c_uint64, ctypes.c_void_p]
        self.dll.xlCanFdSetConfiguration.restype = ctypes.c_int
        self.dll.xlActivateChannel.argtypes = [ctypes.c_long, ctypes.c_uint64, ctypes.c_uint, ctypes.c_uint]
        self.dll.xlActivateChannel.restype = ctypes.c_int
        self.dll.xlCanTransmitEx.argtypes = [
            ctypes.c_long, ctypes.c_uint64, ctypes.c_uint,
            ctypes.POINTER(ctypes.c_uint), ctypes.c_void_p,
        ]
        self.dll.xlCanTransmitEx.restype = ctypes.c_int
        self.dll.xlCanReceive.argtypes = [ctypes.c_long, ctypes.c_void_p]
        self.dll.xlCanReceive.restype = ctypes.c_int

    @staticmethod
    def _check(status, api):
        if status != XL_SUCCESS:
            raise RuntimeError(f"{api} failed, XL status={status}")

    def open(self):
        self._check(self.dll.xlOpenDriver(), "xlOpenDriver")
        permission = ctypes.c_uint64(self.mask)
        self._check(
            self.dll.xlOpenPort(
                ctypes.byref(self.port), b"UDSToolWakeProbe", self.mask,
                ctypes.byref(permission), 262144, XL_INTERFACE_VERSION_V4,
                XL_BUS_TYPE_CAN,
            ),
            "xlOpenPort",
        )
        if permission.value & self.mask:
            timing = XlCanFdConf(
                500000, 16, 63, 16, 2000000, 4, 15, 4, 0, 0,
                (ctypes.c_uint8 * 2)(0, 0), 0,
            )
            self._check(
                self.dll.xlCanFdSetConfiguration(self.port, self.mask, ctypes.byref(timing)),
                "xlCanFdSetConfiguration",
            )
        self._check(
            self.dll.xlActivateChannel(
                self.port, self.mask, XL_BUS_TYPE_CAN, XL_ACTIVATE_RESET_CLOCK
            ),
            "xlActivateChannel",
        )
        now = dt.datetime.now()
        self.asc.write(f"date {now.strftime('%a %b %d %I:%M:%S %p %Y')}\n")
        self.asc.write("base hex  timestamps absolute\ninternal events logged\n")

    def close(self):
        if self.port.value >= 0:
            self.dll.xlDeactivateChannel(self.port, self.mask)
            self.dll.xlClosePort(self.port)
            self.port.value = -1
        self.dll.xlCloseDriver()
        self.asc.close()

    def record(self, direction, can_id, data, fd=False, brs=False):
        elapsed = time.perf_counter() - self.started
        kind = "CANFD" if fd else "CAN"
        line = (
            f"{elapsed:12.6f} {kind} {self.channel} {direction} "
            f"{can_id:X} {len(data)} {format_data(data)}"
        )
        if direction == "Tx" or self.verbose_rx:
            print(line, flush=True)
        self.asc.write(line + "\n")
        self.asc.flush()

    def send(self, can_id, data):
        event = XlCanTxEvent()
        event.tag = XL_CAN_EV_TAG_TX_MSG
        event.transaction_id = 0xFFFF
        event.channel_index = self.channel - 1
        event.tag_data.can_msg.can_id = can_id
        event.tag_data.can_msg.msg_flags = 0
        event.tag_data.can_msg.dlc = len(data)
        for index, value in enumerate(data):
            event.tag_data.can_msg.data[index] = value
        sent = ctypes.c_uint(0)
        self._check(
            self.dll.xlCanTransmitEx(
                self.port, self.mask, 1, ctypes.byref(sent), ctypes.byref(event)
            ),
            "xlCanTransmitEx",
        )
        if sent.value != 1:
            raise RuntimeError("Vector transmitted zero frames")
        self.record("Tx", can_id, data)

    def receive_available(self):
        frames = []
        while True:
            event = XlCanRxEvent()
            status = self.dll.xlCanReceive(self.port, ctypes.byref(event))
            if status != XL_SUCCESS:
                break
            if event.tag != XL_CAN_EV_TAG_RX_OK:
                continue
            message = event.tag_data.can_rx_ok
            length = dlc_length(message.dlc)
            data = bytes(message.data[:length])
            can_id = message.can_id & 0x1FFFFFFF
            fd = bool(message.msg_flags & 1)
            brs = bool(message.msg_flags & 2)
            self.record("Rx", can_id, data, fd, brs)
            frames.append((can_id, data))
        return frames


def main():
    parser = argparse.ArgumentParser(
        description="Non-flashing ChuNeng ARC331 wakeup and physical-session probe on Vector XL"
    )
    parser.add_argument("--channel", type=int, default=2)
    parser.add_argument("--target", choices=("left", "right", "both"), default="both")
    parser.add_argument("--duration", type=float, default=8.0)
    parser.add_argument("--probe-delay", type=float, default=1.0)
    parser.add_argument("--wake-period", type=float, default=0.5)
    parser.add_argument("--probe-period", type=float, default=1.0)
    parser.add_argument("--session", type=lambda value: int(value, 0), default=0x03)
    parser.add_argument("--verbose-rx", action="store_true")
    parser.add_argument("--asc", type=pathlib.Path)
    args = parser.parse_args()

    if not 0 <= args.session <= 0x7F:
        parser.error("--session must be in the range 0x00..0x7F")

    if args.asc is None:
        stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
        args.asc = pathlib.Path(__file__).resolve().parents[1] / "dist" / "logs" / f"chuneng_331_wakeup_probe_{stamp}.asc"
    args.asc.parent.mkdir(parents=True, exist_ok=True)

    wake = bytes.fromhex("00 00 00 00 00 00 00 00")
    probe = bytes((0x02, 0x10, args.session, 0x55, 0x55, 0x55, 0x55, 0x55))
    endpoints = {
        "right": (0x72C, 0x72D),
        "left": (0x72E, 0x72F),
    }
    selected = (
        endpoints.items() if args.target == "both" else ((args.target, endpoints[args.target]),)
    )
    selected = tuple(selected)
    expected_by_rx = {rx_id: name for name, (_, rx_id) in selected}

    print(f"ASC={args.asc}")
    print(
        f"PLAN=CH{args.channel} 500k/2M; 0x520 00x8 @{args.wake_period:.3f}s; "
        f"after {args.probe_delay:.3f}s physical 10 {args.session:02X} @{args.probe_period:.3f}s; "
        f"target={args.target}"
    )
    print("SAFETY=NO 10 02; NO SECURITY ACCESS; NO ERASE; NO DOWNLOAD; NO RESET")
    bus = VectorProbe(args.channel, args.asc, args.verbose_rx)
    valid_counts = {name: 0 for name, _ in selected}
    negative_counts = {name: 0 for name, _ in selected}
    rx_counts = {name: 0 for name, _ in selected}
    try:
        bus.open()
        started = time.perf_counter()
        next_wake = started
        next_probe = started + args.probe_delay
        deadline = started + args.duration
        while time.perf_counter() < deadline:
            now = time.perf_counter()
            if now >= next_wake:
                bus.send(0x520, wake)
                next_wake += args.wake_period
            if now >= next_probe:
                for _, (tx_id, _) in selected:
                    bus.send(tx_id, probe)
                next_probe += args.probe_period
            for can_id, data in bus.receive_available():
                name = expected_by_rx.get(can_id)
                if name is None:
                    continue
                rx_counts[name] += 1
                if len(data) >= 3 and data[0] >= 2 and data[1:3] == bytes((0x50, args.session)):
                    valid_counts[name] += 1
                    print(
                        f"PASS_{name.upper()}_0x{can_id:X}={format_data(data)}",
                        flush=True,
                    )
                elif len(data) >= 4 and data[0] >= 3 and data[1:3] == bytes((0x7F, 0x10)):
                    negative_counts[name] += 1
                    print(
                        f"NRC_{name.upper()}_0x{can_id:X}=0x{data[3]:02X} DATA={format_data(data)}",
                        flush=True,
                    )
                else:
                    print(
                        f"UNEXPECTED_{name.upper()}_0x{can_id:X}={format_data(data)}",
                        flush=True,
                    )
            time.sleep(0.001)
    finally:
        bus.close()
    passed = True
    for name, (_, rx_id) in selected:
        print(
            f"RESULT_{name.upper()}_0x{rx_id:X}: RX={rx_counts[name]} "
            f"VALID_50_{args.session:02X}={valid_counts[name]} NRC={negative_counts[name]}"
        )
        passed = passed and valid_counts[name] > 0
    print(f"FLASH_ACTION=NONE\nOVERALL={'PASS' if passed else 'FAIL'}")
    raise SystemExit(0 if passed else 2)


if __name__ == "__main__":
    main()
