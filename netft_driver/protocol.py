import struct
from dataclasses import dataclass
from enum import IntEnum


REQUEST_HEADER = 0x1234
REQUEST_STRUCT = struct.Struct(">HHI")
RECORD_STRUCT = struct.Struct(">III6i")
RECORD_SIZE = RECORD_STRUCT.size


class Command(IntEnum):
    STOP_STREAMING = 0x0000
    START_REALTIME = 0x0002
    START_BUFFERED = 0x0003
    RESET_CONDITION_LATCH = 0x0041
    SET_SOFTWARE_BIAS = 0x0042


class ProtocolError(ValueError):
    pass


@dataclass(frozen=True)
class RawRecord:
    rdt_sequence: int
    ft_sequence: int
    status: int
    fx: int
    fy: int
    fz: int
    tx: int
    ty: int
    tz: int


def encode_request(command, sample_count=0):
    if not isinstance(sample_count, int) or isinstance(sample_count, bool):
        raise TypeError("sample_count must be an integer")
    if not 0 <= sample_count <= 0xFFFFFFFF:
        raise ValueError("sample_count must fit uint32")
    return REQUEST_STRUCT.pack(REQUEST_HEADER, int(Command(command)), sample_count)


def decode_record(datagram):
    if len(datagram) != RECORD_SIZE:
        raise ProtocolError(
            "RDT record must be exactly {} bytes, received {}".format(
                RECORD_SIZE, len(datagram)
            )
        )
    values = RECORD_STRUCT.unpack(datagram)
    return RawRecord(*values)
