import struct

import pytest

from netft_driver.protocol import Command, encode_request


@pytest.mark.parametrize(
    "command,value",
    [
        (Command.STOP_STREAMING, 0x0000),
        (Command.START_REALTIME, 0x0002),
        (Command.START_BUFFERED, 0x0003),
        (Command.RESET_CONDITION_LATCH, 0x0041),
        (Command.SET_SOFTWARE_BIAS, 0x0042),
    ],
)
def test_encode_request_uses_network_byte_order(command, value):
    assert encode_request(command, 7) == struct.pack(">HHI", 0x1234, value, 7)


@pytest.mark.parametrize("sample_count", [-1, 0x1_0000_0000])
def test_encode_request_rejects_sample_count_outside_uint32(sample_count):
    with pytest.raises(ValueError, match="sample_count"):
        encode_request(Command.START_REALTIME, sample_count)


from netft_driver.protocol import ProtocolError, decode_record


def test_decode_record_preserves_unsigned_headers_and_signed_axes():
    payload = struct.pack(
        ">III6i",
        0xFFFFFFFF,
        0x80000000,
        0x80020000,
        -1,
        2,
        -3,
        4,
        -5,
        6,
    )

    record = decode_record(payload)

    assert record.rdt_sequence == 0xFFFFFFFF
    assert record.ft_sequence == 0x80000000
    assert record.status == 0x80020000
    assert (record.fx, record.fy, record.fz) == (-1, 2, -3)
    assert (record.tx, record.ty, record.tz) == (4, -5, 6)


@pytest.mark.parametrize("size", [0, 7, 35, 37, 72])
def test_decode_record_rejects_non_36_byte_datagrams(size):
    with pytest.raises(ProtocolError, match="exactly 36 bytes"):
        decode_record(bytes(size))
