#!/usr/bin/env python3
import argparse
import json
import signal
import threading
from pathlib import Path

from test.support.fake_sensor import FakeNetFTSensor


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port-file", required=True)
    parser.add_argument("--http-port-file")
    parser.add_argument("--commands-file", required=True)
    parser.add_argument("--rate", type=float, default=200.0)
    arguments = parser.parse_args()
    stopped = threading.Event()

    def request_stop(signum, frame):
        stopped.set()

    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)
    with FakeNetFTSensor(rate_hz=arguments.rate) as sensor:
        Path(arguments.port_file).write_text(str(sensor.port), encoding="utf-8")
        if arguments.http_port_file:
            Path(arguments.http_port_file).write_text(
                str(sensor.http_port), encoding="utf-8"
            )
        stopped.wait()
        commands = [int(command) for command in sensor.commands]
    Path(arguments.commands_file).write_text(
        json.dumps(commands), encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
