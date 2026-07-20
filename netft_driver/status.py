from dataclasses import dataclass
from enum import Enum, IntEnum
from typing import Optional, Tuple


class DiagnosticSeverity(IntEnum):
    OK = 0
    WARN = 1
    ERROR = 2


STATUS_BITS = (
    (0x80000000, "error summary"),
    (0x40000000, "CPU or RAM error"),
    (0x20000000, "digital board error"),
    (0x10000000, "analog board error"),
    (0x08000000, "serial link communication error"),
    (0x04000000, "program memory verification error"),
    (0x02000000, "halted due to configuration errors"),
    (0x01000000, "settings validation error"),
    (0x00800000, "configuration incompatible with calibration"),
    (0x00400000, "network communication failure"),
    (0x00200000, "CAN communication error"),
    (0x00100000, "RDT communication error"),
    (0x00080000, "EtherNet/IP protocol failure"),
    (0x00040000, "DeviceNet protocol failure"),
    (0x00020000, "transducer saturation or A/D error"),
    (0x00010000, "monitor condition latched"),
    (0x00004000, "watchdog timeout error"),
    (0x00002000, "stack check error"),
    (0x00001000, "serial EEPROM I2C failure"),
    (0x00000800, "serial flash SPI failure"),
    (0x00000400, "analog board watchdog timeout"),
    (0x00000200, "excessive strain gage excitation current"),
    (0x00000100, "insufficient strain gage excitation current"),
    (0x00000080, "artificial analog ground out of range"),
    (0x00000040, "analog board power supply too high"),
    (0x00000020, "analog board power supply too low"),
    (0x00000010, "serial link data unavailable"),
    (0x00000008, "reference voltage or power monitoring error"),
    (0x00000004, "internal temperature error"),
    (0x00000002, "HTTP protocol failure"),
)


def decode_status(status):
    if not 0 <= status <= 0xFFFFFFFF:
        raise ValueError("status must fit uint32")
    if status == 0:
        return ("healthy",)
    return tuple(name for mask, name in STATUS_BITS if status & mask)


def classify_status(status):
    if status == 0:
        return DiagnosticSeverity.OK
    if status == 0x80010000:
        return DiagnosticSeverity.WARN
    return DiagnosticSeverity.ERROR


class SequenceKind(Enum):
    FIRST = "first"
    CONTIGUOUS = "contiguous"
    GAP = "gap"
    DUPLICATE = "duplicate"
    OUT_OF_ORDER = "out_of_order"


@dataclass(frozen=True)
class SequenceObservation:
    kind: SequenceKind
    gap: int = 0


class FtSequenceKind(Enum):
    FIRST = "first"
    FORWARD = "forward"
    STALL = "stall"
    BACKWARD = "backward"
    RESTART = "restart"


FT_RESTART_LOW_VALUE_MAX = 0x0000FFFF
FT_RESTART_MIN_BASELINE = FT_RESTART_LOW_VALUE_MAX + 1


@dataclass(frozen=True)
class FtSequenceObservation:
    kind: FtSequenceKind


class FtSequenceTracker:
    """Track uint32 ADC progress without interpreting forward gaps as loss."""

    def __init__(self):
        self._last = None  # type: Optional[int]
        self._restart_candidate = None  # type: Optional[int]

    @property
    def last(self):
        return self._last

    def begin_session(self):
        self._restart_candidate = None

    @staticmethod
    def _validate(value):
        if not isinstance(value, int) or isinstance(value, bool):
            raise TypeError("sequence must be an integer")
        if not 0 <= value <= 0xFFFFFFFF:
            raise ValueError("sequence must fit uint32")

    def observe(self, value):
        self._validate(value)
        if self._last is None:
            self._last = value
            return FtSequenceObservation(FtSequenceKind.FIRST)

        if value == self._last:
            self._restart_candidate = None
            return FtSequenceObservation(FtSequenceKind.STALL)

        delta = (value - self._last) & 0xFFFFFFFF
        if delta < 0x80000000:
            self._last = value
            self._restart_candidate = None
            return FtSequenceObservation(FtSequenceKind.FORWARD)

        if (
            self._last >= FT_RESTART_MIN_BASELINE
            and self._restart_candidate is not None
            and self._restart_candidate <= FT_RESTART_LOW_VALUE_MAX
            and value <= FT_RESTART_LOW_VALUE_MAX
        ):
            candidate_delta = (value - self._restart_candidate) & 0xFFFFFFFF
            if 0 < candidate_delta < 0x80000000:
                self._last = value
                self._restart_candidate = None
                return FtSequenceObservation(FtSequenceKind.RESTART)

        self._restart_candidate = value
        return FtSequenceObservation(FtSequenceKind.BACKWARD)


class RdtSequenceTracker:
    def __init__(self):
        self._last = None  # type: Optional[int]

    @property
    def last(self):
        return self._last

    def reset(self):
        self._last = None

    def observe(self, value):
        if not isinstance(value, int) or isinstance(value, bool):
            raise TypeError("sequence must be an integer")
        if not 0 <= value <= 0xFFFFFFFF:
            raise ValueError("sequence must fit uint32")
        if self._last is None:
            self._last = value
            return SequenceObservation(SequenceKind.FIRST)
        if value == self._last:
            return SequenceObservation(SequenceKind.DUPLICATE)
        expected = (self._last + 1) & 0xFFFFFFFF
        delta = (value - expected) & 0xFFFFFFFF
        if delta == 0:
            self._last = value
            return SequenceObservation(SequenceKind.CONTIGUOUS)
        if delta < 0x80000000:
            self._last = value
            return SequenceObservation(SequenceKind.GAP, gap=delta)
        return SequenceObservation(SequenceKind.OUT_OF_ORDER)
