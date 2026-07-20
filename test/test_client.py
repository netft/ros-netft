import math
import socket
import threading
import time
from contextlib import contextmanager

import pytest

from netft_driver.client import ClientConfig, ClientState, NetFTClient
from netft_driver.protocol import Command, RawRecord
from test.support.fake_sensor import FakeNetFTSensor


def valid_config(**overrides):
    values = {
        "sensor_host": "127.0.0.1",
        "sensor_port": 49152,
        "counts_per_force": 1_000_000.0,
        "counts_per_torque": 1_000_000.0,
        "publish_rate": 0.0,
        "receive_timeout": 0.1,
        "reconnect_initial_delay": 0.01,
        "reconnect_max_delay": 0.05,
    }
    values.update(overrides)
    return ClientConfig(**values)


@pytest.mark.parametrize(
    "overrides,match",
    [
        ({"sensor_host": ""}, "sensor_host"),
        ({"sensor_port": 0}, "sensor_port"),
        ({"sensor_port": 65536}, "sensor_port"),
        ({"counts_per_force": 0.0}, "counts_per_force"),
        ({"counts_per_torque": math.inf}, "counts_per_torque"),
        ({"publish_rate": -1.0}, "publish_rate"),
        ({"receive_timeout": 0.0}, "receive_timeout"),
        ({"reconnect_initial_delay": 0.0}, "reconnect_initial_delay"),
        (
            {"reconnect_initial_delay": 2.0, "reconnect_max_delay": 1.0},
            "reconnect_max_delay",
        ),
    ],
)
def test_client_config_rejects_invalid_values(overrides, match):
    with pytest.raises((TypeError, ValueError), match=match):
        valid_config(**overrides)


def test_client_starts_with_stopped_health():
    snapshot = NetFTClient(valid_config()).health_snapshot()
    assert snapshot.state is ClientState.STOPPED
    assert snapshot.sensor_host == "127.0.0.1"
    assert snapshot.received_count == 0
    assert snapshot.last_record_age is None


def test_record_conversion_uses_independent_force_and_torque_scales():
    client = NetFTClient(
        valid_config(counts_per_force=100.0, counts_per_torque=10.0)
    )
    record = RawRecord(1, 2, 0, 100, -200, 50, 10, -20, 5)
    sample = client._convert_record(record, received_monotonic=12.5)
    assert sample.force == (1.0, -2.0, 0.5)
    assert sample.torque == (1.0, -2.0, 0.5)
    assert sample.received_monotonic == 12.5


def wait_until(predicate, timeout=1.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.005)
    raise AssertionError(
        "condition did not become true within {:.3f}s".format(timeout)
    )


def sensor_config(sensor, **overrides):
    overrides.setdefault("receive_timeout", 0.05)
    return valid_config(
        sensor_host=sensor.host, sensor_port=sensor.port, **overrides
    )


@contextmanager
def running_client(client, callback):
    client.start(callback)
    try:
        yield client
    finally:
        client.stop()


def test_client_streams_scaled_samples_and_stops_cleanly():
    samples = []
    with FakeNetFTSensor(rate_hz=200.0) as sensor:
        client = NetFTClient(sensor_config(sensor))
        with running_client(client, samples.append):
            wait_until(lambda: len(samples) >= 3)

        assert samples[0].force == (0.0001, -0.0002, 0.0003)
        assert samples[0].torque == (0.00001, -0.00002, 0.00003)
        assert sensor.wait_for_command(Command.STOP_STREAMING)
        assert sensor.requests == (
            (Command.START_REALTIME, 0),
            (Command.STOP_STREAMING, 0),
        )
        assert client.health_snapshot().state is ClientState.STOPPED


def test_stop_is_idempotent_before_and_after_start():
    with FakeNetFTSensor() as sensor:
        client = NetFTClient(sensor_config(sensor))
        client.stop()
        with running_client(client, lambda sample: None):
            wait_until(lambda: client.health_snapshot().received_count > 0)
        client.stop()


def test_bias_command_is_followed_by_stream_restart():
    with FakeNetFTSensor(rate_hz=200.0) as sensor:
        client = NetFTClient(sensor_config(sensor))
        with running_client(client, lambda sample: None):
            wait_until(
                lambda: client.health_snapshot().state is ClientState.STREAMING
            )
            client.bias()
            assert sensor.wait_for_command(Command.SET_SOFTWARE_BIAS)
            assert sensor.wait_for_command(Command.START_REALTIME, count=2)
            wait_until(lambda: client.health_snapshot().received_count >= 2)
        assert sensor.wait_for_command(Command.STOP_STREAMING)
        assert sensor.requests == (
            (Command.START_REALTIME, 0),
            (Command.SET_SOFTWARE_BIAS, 0),
            (Command.START_REALTIME, 0),
            (Command.STOP_STREAMING, 0),
        )


def test_publish_rate_drops_intermediate_samples_without_queueing():
    samples = []
    with FakeNetFTSensor(rate_hz=500.0) as sensor:
        client = NetFTClient(sensor_config(sensor, publish_rate=50.0))
        with running_client(client, samples.append):
            time.sleep(0.25)
        health = client.health_snapshot()
        assert 5 <= len(samples) <= 20
        assert health.received_count > health.published_count
        assert health.rate_dropped_count > 0
        assert samples[-1].rdt_sequence > samples[0].rdt_sequence


def test_bias_fails_while_not_connected():
    client = NetFTClient(valid_config())
    with pytest.raises(ConnectionError, match="not streaming"):
        client.bias()


def test_running_client_cleans_up_after_assertion_failure():
    sensor = FakeNetFTSensor(rate_hz=200.0).start()
    client = NetFTClient(sensor_config(sensor))
    try:
        with pytest.raises(AssertionError, match="forced assertion failure"):
            with running_client(client, lambda sample: None):
                wait_until(
                    lambda: client.health_snapshot().received_count > 0
                )
                raise AssertionError("forced assertion failure")
        assert client._socket is None
        assert client._thread is None or not client._thread.is_alive()
    finally:
        client.stop()
        sensor.close()
    assert sensor._thread is not None
    assert not sensor._thread.is_alive()


def test_repeated_immediate_stop_never_leaves_remote_streaming():
    clients = []
    with FakeNetFTSensor(rate_hz=500.0) as sensor:
        for _ in range(50):
            client = NetFTClient(sensor_config(sensor, receive_timeout=0.01))
            clients.append(client)
            try:
                client.start(lambda sample: None)
            finally:
                client.stop()
            assert client._socket is None
            assert client._thread is None or not client._thread.is_alive()

        time.sleep(0.05)
        requests = sensor.requests
        starts = requests.count((Command.START_REALTIME, 0))
        stops = requests.count((Command.STOP_STREAMING, 0))
        assert starts == stops
        assert not sensor._streaming
        assert len(requests) % 2 == 0
        for index in range(0, len(requests), 2):
            assert requests[index : index + 2] == (
                (Command.START_REALTIME, 0),
                (Command.STOP_STREAMING, 0),
            )


def test_callback_lifecycle_calls_reject_restart_until_worker_unwinds():
    callback_waiting = threading.Event()
    release_callback = threading.Event()
    observations = {}
    with FakeNetFTSensor(rate_hz=200.0) as sensor:
        client = NetFTClient(sensor_config(sensor))

        def callback(sample):
            observations["worker"] = threading.current_thread()
            try:
                observations["health"] = client.health_snapshot()
                client.bias()
                client.stop()
                try:
                    client.start(lambda replacement_sample: None)
                except Exception as exc:
                    observations["restart_error"] = exc
            except Exception as exc:
                observations["callback_error"] = exc
            finally:
                callback_waiting.set()
                release_callback.wait(timeout=2.0)

        client.start(callback)
        workers = []
        try:
            assert callback_waiting.wait(timeout=1.0)
            worker = observations["worker"]
            workers = [worker, client._thread]
            assert "callback_error" not in observations
            assert observations["health"].state is ClientState.STREAMING
            assert isinstance(observations.get("restart_error"), RuntimeError)
            assert client._thread is worker
            assert worker.is_alive()
            assert client._socket is None
            assert client.health_snapshot().state is not ClientState.STOPPED
        finally:
            release_callback.set()
            for worker in workers:
                if worker is not None and worker is not threading.current_thread():
                    worker.join(timeout=2.0)
            client.stop()

        assert sensor.wait_for_command(Command.STOP_STREAMING)
        assert sensor.requests == (
            (Command.START_REALTIME, 0),
            (Command.SET_SOFTWARE_BIAS, 0),
            (Command.START_REALTIME, 0),
            (Command.STOP_STREAMING, 0),
        )
        assert client._thread is None
        assert client._socket is None
        assert client.health_snapshot().state is ClientState.STOPPED


def test_stop_timeout_retains_live_worker_ownership():
    callback_entered = threading.Event()
    release_callback = threading.Event()
    with FakeNetFTSensor(rate_hz=200.0) as sensor:
        client = NetFTClient(sensor_config(sensor, receive_timeout=0.01))

        def blocking_callback(sample):
            callback_entered.set()
            release_callback.wait(timeout=3.0)

        client.start(blocking_callback)
        worker = None
        try:
            assert callback_entered.wait(timeout=1.0)
            worker = client._thread
            client.stop()
            assert worker is not None
            assert worker.is_alive()
            assert client._thread is worker
            assert client._socket is None
            assert client.health_snapshot().state is not ClientState.STOPPED
            with pytest.raises(RuntimeError, match="already running"):
                client.start(lambda sample: None)
        finally:
            release_callback.set()
            if worker is not None:
                worker.join(timeout=2.0)
            client.stop()

        assert sensor.wait_for_command(Command.STOP_STREAMING)
        assert sensor.requests == (
            (Command.START_REALTIME, 0),
            (Command.STOP_STREAMING, 0),
        )
        assert client._thread is None
        assert client._socket is None
        assert client.health_snapshot().state is ClientState.STOPPED


def test_malformed_and_serious_status_records_are_counted_and_not_published():
    samples = []
    with FakeNetFTSensor(rate_hz=100.0) as sensor:
        sensor.queue_payload(b"short")
        sensor.queue_record(1, status=0x80020000)
        client = NetFTClient(sensor_config(sensor))
        with running_client(client, samples.append):
            wait_until(lambda: client.health_snapshot().malformed_count >= 1)
            wait_until(lambda: client.health_snapshot().device_error_count >= 1)
            wait_until(lambda: len(samples) >= 1)
        health = client.health_snapshot()
        assert health.malformed_count >= 1
        assert health.device_error_count >= 1
        assert all(sample.status == 0 for sample in samples)


def test_sequence_gap_duplicate_and_out_of_order_are_accounted():
    with FakeNetFTSensor(rate_hz=100.0) as sensor:
        sensor.queue_record(10)
        sensor.queue_record(14)
        sensor.queue_record(14)
        sensor.queue_record(13)
        client = NetFTClient(sensor_config(sensor))
        with running_client(client, lambda sample: None):
            wait_until(lambda: client.health_snapshot().out_of_order_count >= 1)
        health = client.health_snapshot()
        assert health.lost_count == 3
        assert health.duplicate_count == 1
        assert health.out_of_order_count == 1


def test_ft_sequence_progress_accepts_gaps_and_rollover_without_network_loss():
    published = []
    client = NetFTClient(valid_config())
    client._callback = published.append
    client._handle_record(
        RawRecord(1, 0xFFFFFFF0, 0, 1, 2, 3, 4, 5, 6), 1.0
    )
    client._handle_record(
        RawRecord(2, 0xFFFFFFFE, 0, 1, 2, 3, 4, 5, 6), 1.1
    )
    client._handle_record(RawRecord(3, 7, 0, 1, 2, 3, 4, 5, 6), 1.2)

    health = client.health_snapshot()
    assert len(published) == 3
    assert health.lost_count == 0
    assert health.ft_stall_count == 0
    assert health.ft_backward_count == 0
    assert health.ft_restart_count == 0
    assert health.last_ft_progress == "forward"


def test_ft_sequence_stall_and_backward_are_counted_but_still_published():
    published = []
    client = NetFTClient(valid_config())
    client._callback = published.append
    client._handle_record(RawRecord(1, 100, 0, 1, 2, 3, 4, 5, 6), 1.0)
    client._handle_record(RawRecord(2, 100, 0, 1, 2, 3, 4, 5, 6), 1.1)
    client._handle_record(RawRecord(3, 90, 0, 1, 2, 3, 4, 5, 6), 1.2)

    health = client.health_snapshot()
    assert len(published) == 3
    assert health.ft_stall_count == 1
    assert health.ft_backward_count == 1
    assert health.ft_restart_count == 0
    assert health.last_ft_progress == "backward"


def test_ft_sequence_continuity_and_restart_survive_rdt_tracker_resets():
    client = NetFTClient(valid_config())
    client._callback = lambda sample: None
    baseline = 0x00100000
    client._handle_record(RawRecord(20, baseline, 0, 1, 2, 3, 4, 5, 6), 1.0)

    client._sequence_tracker.reset()
    client._ft_sequence_tracker.begin_session()
    client._handle_record(
        RawRecord(1, baseline + 4, 0, 1, 2, 3, 4, 5, 6), 1.1
    )
    continuous = client.health_snapshot()
    assert continuous.ft_restart_count == 0
    assert continuous.last_ft_progress == "forward"

    client._sequence_tracker.reset()
    client._ft_sequence_tracker.begin_session()
    client._handle_record(RawRecord(1, 2, 0, 1, 2, 3, 4, 5, 6), 1.2)
    unconfirmed = client.health_snapshot()
    assert unconfirmed.ft_backward_count == 1
    assert unconfirmed.ft_restart_count == 0
    assert unconfirmed.last_ft_progress == "backward"

    client._handle_record(RawRecord(2, 6, 0, 1, 2, 3, 4, 5, 6), 1.3)
    restarted = client.health_snapshot()
    assert restarted.ft_backward_count == 1
    assert restarted.ft_restart_count == 1
    assert restarted.last_ft_progress == "restart"


def test_ft_sequence_continues_through_real_loopback_reconnect():
    samples = []
    with FakeNetFTSensor(rate_hz=200.0) as sensor:
        sensor.queue_record(10, ft_sequence=1000)

        def pause_after_each_target(sample):
            samples.append(sample)
            if len(samples) in (1, 2):
                sensor.pause()

        client = NetFTClient(sensor_config(sensor, receive_timeout=0.03))
        with running_client(client, pause_after_each_target):
            wait_until(lambda: len(samples) == 1)
            wait_until(lambda: client.health_snapshot().reconnect_count >= 1)
            assert sensor.wait_for_command(Command.START_REALTIME, count=2)
            sensor.queue_record(1, ft_sequence=1004)
            sensor.resume()
            wait_until(lambda: len(samples) == 2)
            health = client.health_snapshot()

        assert health.ft_backward_count == 0
        assert health.ft_restart_count == 0
        assert health.last_ft_progress == "forward"
        assert health.lost_count == 0


def test_real_loopback_reconnect_clears_unconfirmed_ft_restart_candidate():
    samples = []
    with FakeNetFTSensor(rate_hz=200.0) as sensor:
        sensor.queue_record(10, ft_sequence=0x00100000)
        sensor.queue_record(11, ft_sequence=2)

        def pause_at_observation_boundaries(sample):
            samples.append(sample)
            if len(samples) in (2, 3, 4):
                sensor.pause()

        client = NetFTClient(sensor_config(sensor, receive_timeout=0.03))
        with running_client(client, pause_at_observation_boundaries):
            wait_until(lambda: len(samples) == 2)
            before_reconnect = client.health_snapshot()
            assert before_reconnect.ft_backward_count == 1
            assert before_reconnect.ft_restart_count == 0

            wait_until(lambda: client.health_snapshot().reconnect_count >= 1)
            assert sensor.wait_for_command(Command.START_REALTIME, count=2)
            sensor.queue_record(1, ft_sequence=6)
            sensor.queue_record(2, ft_sequence=10)
            sensor.resume()
            wait_until(lambda: len(samples) == 3)
            unconfirmed = client.health_snapshot()
            assert unconfirmed.ft_backward_count == 2
            assert unconfirmed.ft_restart_count == 0

            sensor.resume()
            wait_until(lambda: len(samples) == 4)
            confirmed = client.health_snapshot()

        assert confirmed.ft_backward_count == 2
        assert confirmed.ft_restart_count == 1
        assert confirmed.last_ft_progress == "restart"


def test_real_loopback_reconnect_rejects_high_backward_restart_candidates():
    samples = []
    with FakeNetFTSensor(rate_hz=200.0) as sensor:
        sensor.queue_record(10, ft_sequence=0x70000000)

        def pause_at_targets(sample):
            samples.append(sample)
            if len(samples) in (1, 3):
                sensor.pause()

        client = NetFTClient(sensor_config(sensor, receive_timeout=0.03))
        with running_client(client, pause_at_targets):
            wait_until(lambda: len(samples) == 1)
            wait_until(lambda: client.health_snapshot().reconnect_count >= 1)
            assert sensor.wait_for_command(Command.START_REALTIME, count=2)
            sensor.queue_record(1, ft_sequence=0x6FFFFF00)
            sensor.queue_record(2, ft_sequence=0x6FFFFF04)
            sensor.resume()
            wait_until(lambda: len(samples) == 3)
            health = client.health_snapshot()

        assert health.ft_backward_count == 2
        assert health.ft_restart_count == 0
        assert health.last_ft_progress == "backward"
        assert client._ft_sequence_tracker.last == 0x70000000


def test_publish_on_error_allows_serious_status_record():
    samples = []
    with FakeNetFTSensor(rate_hz=100.0) as sensor:
        sensor.queue_record(1, status=0x80020000)
        client = NetFTClient(sensor_config(sensor, publish_on_error=True))
        with running_client(client, samples.append):
            wait_until(
                lambda: any(sample.status == 0x80020000 for sample in samples)
            )


def test_duplicate_rdt_record_still_counts_serious_device_status_once():
    published = []
    client = NetFTClient(valid_config(publish_on_error=True))
    client._callback = published.append
    client._handle_record(RawRecord(10, 100, 0, 1, 2, 3, 4, 5, 6), 1.0)
    client._handle_record(
        RawRecord(10, 100, 0x80020000, 1, 2, 3, 4, 5, 6), 1.1
    )

    health = client.health_snapshot()
    assert health.received_count == 2
    assert health.duplicate_count == 1
    assert health.device_error_count == 1
    assert len(published) == 1


def test_out_of_order_rdt_record_still_counts_serious_device_status_once():
    published = []
    client = NetFTClient(valid_config(publish_on_error=True))
    client._callback = published.append
    client._handle_record(RawRecord(10, 100, 0, 1, 2, 3, 4, 5, 6), 1.0)
    client._handle_record(
        RawRecord(9, 96, 0x80020000, 1, 2, 3, 4, 5, 6), 1.1
    )

    health = client.health_snapshot()
    assert health.received_count == 2
    assert health.out_of_order_count == 1
    assert health.device_error_count == 1
    assert len(published) == 1


def test_timeout_enters_backoff_reconnects_and_recovers():
    samples = []
    with FakeNetFTSensor(rate_hz=200.0) as sensor:
        client = NetFTClient(sensor_config(sensor, receive_timeout=0.03))
        with running_client(client, samples.append):
            wait_until(lambda: len(samples) >= 2)
            sensor.pause()
            wait_until(lambda: client.health_snapshot().timeout_count >= 1)
            wait_until(lambda: client.health_snapshot().reconnect_count >= 1)
            sensor.resume()
            previous = len(samples)
            wait_until(lambda: len(samples) > previous, timeout=2.0)
            assert sensor.wait_for_command(Command.START_REALTIME, count=2)


def test_callback_exception_is_recorded_without_killing_receiver():
    calls = []

    def callback(sample):
        calls.append(sample.rdt_sequence)
        if len(calls) == 1:
            raise RuntimeError("consumer failed")

    with FakeNetFTSensor(rate_hz=200.0) as sensor:
        client = NetFTClient(sensor_config(sensor))
        with running_client(client, callback):
            wait_until(lambda: len(calls) >= 3)
            health = client.health_snapshot()
        assert health.callback_error_count == 1
        assert "consumer failed" in health.last_error
        assert health.published_count >= 2


def test_two_kilohertz_stream_remains_bounded_and_stoppable():
    samples = []
    with FakeNetFTSensor(rate_hz=2000.0) as sensor:
        client = NetFTClient(sensor_config(sensor, receive_timeout=0.1))
        with running_client(client, samples.append):
            time.sleep(0.5)
        health = client.health_snapshot()
        assert health.received_count >= 200
        assert health.published_count == len(samples)
        assert health.state is ClientState.STOPPED
        assert len(client._receive_times) <= 2500


def test_rate_windows_discard_entries_older_than_one_second():
    client = NetFTClient(valid_config())
    client._callback = lambda sample: None
    for sequence in range(4000):
        record = RawRecord(
            sequence, sequence * 4, 0, 1, 2, 3, 4, 5, 6
        )
        client._handle_record(record, sequence / 2000.0)
    assert 1900 <= len(client._receive_times) <= 2001
    assert 1900 <= len(client._publish_times) <= 2001


def test_backoff_resets_after_valid_recovery_and_remains_capped():
    client = NetFTClient(
        valid_config(
            reconnect_initial_delay=0.01,
            reconnect_max_delay=0.05,
        )
    )
    client._callback = lambda sample: None
    delays = []
    attempts = [0]

    class RecordingStopEvent:
        def __init__(self):
            self.stopped = False

        def is_set(self):
            return self.stopped

        def wait(self, delay):
            delays.append(delay)
            if len(delays) == 6:
                self.stopped = True
                return True
            return False

    def scripted_receive(stop_event):
        attempts[0] += 1
        if attempts[0] == 3:
            record = RawRecord(1, 4, 0, 1, 2, 3, 4, 5, 6)
            client._handle_record(record, time.monotonic())
        raise socket.timeout("scripted receive failure")

    client._receive_session = scripted_receive
    client._run(RecordingStopEvent())

    assert delays == pytest.approx([0.01, 0.02, 0.01, 0.02, 0.04, 0.05])
    health = client.health_snapshot()
    assert health.timeout_count == 6
    assert health.reconnect_count == 6


def test_sparse_malformed_record_does_not_extend_valid_record_deadline():
    samples = []
    receive_timeout = 0.3
    with FakeNetFTSensor(rate_hz=20.0) as sensor:
        client = NetFTClient(
            sensor_config(sensor, receive_timeout=receive_timeout)
        )
        with running_client(client, samples.append):
            wait_until(lambda: len(samples) >= 1)
            sensor.pause()
            last_valid = samples[-1].received_monotonic
            malformed_at = last_valid + 0.2
            while time.monotonic() < malformed_at:
                time.sleep(0.002)
            sensor.send_payload_now(b"short")
            wait_until(lambda: client.health_snapshot().malformed_count == 1)
            wait_until(
                lambda: client.health_snapshot().timeout_count == 1,
                timeout=0.7,
            )
            timeout_elapsed = time.monotonic() - last_valid
            health = client.health_snapshot()

        assert 0.25 <= timeout_elapsed < 0.4
        assert health.malformed_count == 1
        assert health.timeout_count == 1
        assert health.reconnect_count == 1
        assert health.last_error == "no valid RDT record before timeout"


def test_stop_interrupts_large_receive_timeout_without_fault_drift():
    with FakeNetFTSensor(rate_hz=20.0) as sensor:
        client = NetFTClient(sensor_config(sensor, receive_timeout=2.0))
        client.start(lambda sample: None)
        try:
            wait_until(lambda: client.health_snapshot().received_count >= 1)
            sensor.pause()
            time.sleep(0.06)
            before = client.health_snapshot()
            started = time.monotonic()
            client.stop()
            stop_elapsed = time.monotonic() - started
        finally:
            client.stop()

        assert stop_elapsed < 0.25
        assert sensor.wait_for_command(Command.STOP_STREAMING)
        assert sensor.requests == (
            (Command.START_REALTIME, 0),
            (Command.STOP_STREAMING, 0),
        )
        health = client.health_snapshot()
        assert health.received_count == before.received_count
        assert health.published_count == before.published_count
        assert health.rate_dropped_count == before.rate_dropped_count
        assert health.device_error_count == before.device_error_count == 0
        assert health.lost_count == before.lost_count == 0
        assert health.duplicate_count == before.duplicate_count == 0
        assert health.out_of_order_count == before.out_of_order_count == 0
        assert health.timeout_count == before.timeout_count == 0
        assert health.reconnect_count == before.reconnect_count == 0
        assert health.malformed_count == before.malformed_count == 0
        assert health.callback_error_count == before.callback_error_count == 0
        assert health.last_error == before.last_error == ""
        assert client._socket is None
        assert client._thread is None
        assert health.state is ClientState.STOPPED
