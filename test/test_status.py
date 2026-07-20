import pytest

from netft_driver.status import (
    DiagnosticSeverity,
    FtSequenceKind,
    FtSequenceTracker,
    classify_status,
    decode_status,
)


def test_status_severity_distinguishes_health_condition_and_error():
    assert classify_status(0x00000000) is DiagnosticSeverity.OK
    assert classify_status(0x80010000) is DiagnosticSeverity.WARN
    assert classify_status(0x80020000) is DiagnosticSeverity.ERROR
    assert classify_status(0x00010000) is DiagnosticSeverity.ERROR


@pytest.mark.parametrize(
    "mask,name",
    [
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
    ],
)
def test_decode_status_names_every_defined_active_bit(mask, name):
    assert name in decode_status(mask)


def test_decode_status_reports_healthy_when_no_bits_are_set():
    assert decode_status(0) == ("healthy",)


from netft_driver.status import RdtSequenceTracker, SequenceKind


def test_sequence_tracker_classifies_first_contiguous_and_rollover():
    tracker = RdtSequenceTracker()
    assert tracker.observe(0xFFFFFFFE).kind is SequenceKind.FIRST
    assert tracker.observe(0xFFFFFFFF).kind is SequenceKind.CONTIGUOUS
    assert tracker.observe(0).kind is SequenceKind.CONTIGUOUS


def test_sequence_tracker_counts_forward_gap_exactly():
    tracker = RdtSequenceTracker()
    tracker.observe(10)
    observation = tracker.observe(14)
    assert observation.kind is SequenceKind.GAP
    assert observation.gap == 3


def test_sequence_tracker_distinguishes_duplicate_and_out_of_order():
    tracker = RdtSequenceTracker()
    tracker.observe(100)
    assert tracker.observe(100).kind is SequenceKind.DUPLICATE
    assert tracker.observe(99).kind is SequenceKind.OUT_OF_ORDER
    assert tracker.observe(101).kind is SequenceKind.CONTIGUOUS


def test_sequence_tracker_reset_starts_a_new_session():
    tracker = RdtSequenceTracker()
    tracker.observe(50)
    tracker.reset()
    assert tracker.observe(1).kind is SequenceKind.FIRST


def test_ft_sequence_accepts_arbitrary_forward_gaps_and_rollover():
    tracker = FtSequenceTracker()
    assert tracker.observe(0xFFFFFFF0).kind is FtSequenceKind.FIRST
    assert tracker.observe(0xFFFFFFFE).kind is FtSequenceKind.FORWARD
    assert tracker.observe(7).kind is FtSequenceKind.FORWARD


def test_ft_sequence_distinguishes_stall_and_backward_without_moving_baseline():
    tracker = FtSequenceTracker()
    tracker.observe(100)
    assert tracker.observe(100).kind is FtSequenceKind.STALL
    assert tracker.observe(90).kind is FtSequenceKind.BACKWARD
    assert tracker.observe(104).kind is FtSequenceKind.FORWARD


def test_ft_sequence_continues_forward_across_a_new_rdt_session():
    tracker = FtSequenceTracker()
    tracker.observe(1000)
    tracker.begin_session()
    assert tracker.observe(1004).kind is FtSequenceKind.FORWARD


def test_ft_sequence_requires_progress_confirmation_after_new_session_backward():
    tracker = FtSequenceTracker()
    tracker.observe(0x00100000)
    tracker.begin_session()
    assert tracker.observe(2).kind is FtSequenceKind.BACKWARD
    assert tracker.observe(6).kind is FtSequenceKind.RESTART
    assert tracker.observe(10).kind is FtSequenceKind.FORWARD


def test_ft_sequence_confirms_in_session_restart_from_two_low_records():
    tracker = FtSequenceTracker()
    tracker.observe(0x00100000)
    assert tracker.observe(2).kind is FtSequenceKind.BACKWARD
    assert tracker.observe(6).kind is FtSequenceKind.RESTART
    assert tracker.observe(10).kind is FtSequenceKind.FORWARD


def test_ft_restart_candidate_is_cleared_by_session_or_baseline_progress():
    tracker = FtSequenceTracker()
    baseline = 0x00100000
    tracker.observe(baseline)
    assert tracker.observe(2).kind is FtSequenceKind.BACKWARD
    tracker.begin_session()
    assert tracker.observe(6).kind is FtSequenceKind.BACKWARD
    assert tracker.observe(baseline + 4).kind is FtSequenceKind.FORWARD
    assert tracker.observe(10).kind is FtSequenceKind.BACKWARD
    assert tracker.observe(14).kind is FtSequenceKind.RESTART


def test_ft_sequence_does_not_restart_from_progress_just_below_high_baseline():
    tracker = FtSequenceTracker()
    baseline = 0x70000000
    tracker.observe(baseline)
    assert tracker.observe(0x6FFFFF00).kind is FtSequenceKind.BACKWARD
    assert tracker.observe(0x6FFFFF04).kind is FtSequenceKind.BACKWARD
    assert tracker.last == baseline
