import socket
import urllib.request
import xml.etree.ElementTree as ET

from test.support.fake_sensor import (
    Command,
    FakeNetFTSensor,
    RECORD_STRUCT,
    REQUEST_STRUCT,
)


def _request(command):
    return REQUEST_STRUCT.pack(0x1234, int(command), 0)


def _receive_axes(client):
    payload = client.recv(RECORD_STRUCT.size)
    return RECORD_STRUCT.unpack(payload)[3:]


def test_fake_sensor_applies_software_bias_to_subsequent_records():
    with FakeNetFTSensor(rate_hz=200.0) as sensor:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
            client.settimeout(1.0)
            client.connect((sensor.host, sensor.port))
            client.send(_request(Command.START_REALTIME))
            assert _receive_axes(client) == (100, -200, 300, 10, -20, 30)

            client.send(_request(Command.SET_SOFTWARE_BIAS))
            client.send(_request(Command.START_REALTIME))
            for _ in range(20):
                if _receive_axes(client) == (0, 0, 0, 0, 0, 0):
                    break
            else:
                raise AssertionError("fake sensor did not apply its software bias")


def test_fake_sensor_serves_non_si_calibration_on_loopback_http():
    with FakeNetFTSensor(rate_hz=200.0) as sensor:
        with urllib.request.urlopen(
            f"http://{sensor.host}:{sensor.http_port}/netftapi2.xml", timeout=1.0
        ) as response:
            configuration = ET.fromstring(response.read())

    assert configuration.findtext("prodname") == "Fake Net F/T"
    assert configuration.findtext("cfgcpf") == "1000"
    assert configuration.findtext("cfgcpt") == "10"
    assert configuration.findtext("scfgfu") == "kN"
    assert configuration.findtext("scfgtu") == "N-mm"
