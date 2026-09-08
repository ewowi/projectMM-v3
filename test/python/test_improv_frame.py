# /// script
# dependencies = ["pytest", "pyserial"]
# ///
"""Improv frame-contract tests (Python side).

Pins the wire format the device C++ (src/core/ImprovFrame.h), the installer JS
(mooninstaller/improv-frame.js), and this Python builder (moondeck/build/improv_provision.py)
must all agree on byte-for-byte. The G1 golden vector below is the SAME one asserted
in test/js/improv-frame.test.mjs, so the JS and Python envelope builders can't drift;
it's hand-verified against the C++ sum-mod-256 checksum too.

pyserial is an inline dep only because improv_provision.py's `import serial` guard
sys.exit()s when it's missing — the frame functions themselves need nothing. Run:
`uv run pytest test/python` (uv honours the inline deps above).

The config push (the APPLY_OP op planner and its chunked framing) exists in JS
(mooninstaller/config-ops.js, improv-frame.js) and in the Python script; the planner cases
below mirror test/js/config-ops.test.mjs one for one so the two cannot drift.
"""

import sys
from pathlib import Path

# improv_provision.py lives in moondeck/build and imports a sibling (host_wifi).
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "moondeck" / "build"))

import improv_provision as ip  # noqa: E402


def test_checksum_is_sum_mod_256():
    assert ip.checksum(b"abc") == sum(b"abc") & 0xFF
    assert ip.checksum(b"") == 0
    assert ip.checksum(bytes([0xFF, 0xFF])) == 0xFE  # wraps mod 256


def test_frame_layout():
    frame = ip.build_frame(0x03, bytes([0x01]))
    assert frame[0:6] == b"IMPROV", "magic"
    assert frame[6] == 0x01, "version"
    assert frame[7] == 0x03, "type"
    assert frame[8] == 1, "length"
    assert frame[9] == 0x01, "payload"
    assert len(frame) == 11, "9 header + 1 payload + 1 checksum"


def test_golden_vector_g1():
    # Shared with test/js G1. Hand-verified checksum 0xe3.
    frame = ip.build_frame(0x03, bytes([0x01]))
    assert frame.hex(" ") == "49 4d 50 52 4f 56 01 03 01 01 e3"


def test_checksum_covers_header_through_payload():
    payload = bytes([0xAA, 0xBB, 0xCC])
    frame = ip.build_frame(0x03, payload)
    assert frame[-1] == sum(frame[:-1]) & 0xFF


# --- device-model config push: the planner mirrors config-ops.js, the framing the device ---

from improv_provision import (plan_config_ops, encode_apply_op_frames, APPLY_OP_CHUNK_MAX,
                              IMPROV_CMD_APPLY_OP)

S3_LIKE = {"name": "x", "modules": [
    {"type": "System", "id": "System", "controls": {"deviceModel": "x"}},
    {"type": "GridLayout", "id": "Grid", "parent_id": "Layouts", "controls": {"width": 8}},
    {"type": "Layer", "id": "Layer", "parent_id": "Effects"},
    {"type": "NoiseEffect", "id": "Noise", "parent_id": "Layer"},
    {"type": "RmtLedDriver", "id": "RmtLed", "parent_id": "Drivers", "controls": {"pins": "16"}},
]}


def test_every_add_parent_is_cleared_unless_the_parent_is_itself_added_fresh():
    ops = plan_config_ops(S3_LIKE)
    cleared = {o["parent"] for o in ops if o["op"] == "clearChildren"}
    assert cleared == {"Layouts", "Effects", "Drivers"}   # Layer is added fresh: not cleared


def test_all_clearchildren_ops_come_before_all_add_and_set_ops():
    ops = plan_config_ops(S3_LIKE)
    kinds = [o["op"] for o in ops]
    last_clear = max(i for i, k in enumerate(kinds) if k == "clearChildren")
    first_other = min(i for i, k in enumerate(kinds) if k != "clearChildren")
    assert last_clear < first_other


def test_a_modules_add_precedes_its_own_set_ops():
    ops = plan_config_ops(S3_LIKE)
    i_add = next(i for i, o in enumerate(ops) if o["op"] == "add" and o["id"] == "RmtLed")
    i_set = next(i for i, o in enumerate(ops) if o["op"] == "set" and o["module"] == "RmtLed")
    assert i_add < i_set
    assert ops[i_set] == {"op": "set", "module": "RmtLed", "control": "pins", "value": "16"}


def test_the_device_model_name_rides_as_an_ordinary_set_on_system():
    ops = plan_config_ops(S3_LIKE)
    assert {"op": "set", "module": "System", "control": "deviceModel", "value": "x"} in ops
    assert not any(o["op"] == "add" and o["id"] == "System" for o in ops)   # never re-added


def test_a_deduped_parent_is_cleared_exactly_once():
    entry = {"modules": [{"type": "A", "id": "a", "parent_id": "Drivers"},
                         {"type": "B", "id": "b", "parent_id": "Drivers"}]}
    ops = plan_config_ops(entry)
    assert [o for o in ops if o["op"] == "clearChildren"] == [{"op": "clearChildren", "parent": "Drivers"}]


def test_empty_or_malformed_entry_yields_no_ops():
    for entry in (None, {}, {"modules": None}, {"modules": [None, 3, {"id": ""}, {"type": "X"}]}):
        assert plan_config_ops(entry) == []


def test_an_op_is_one_frame_when_it_fits_and_chunks_in_order_when_it_does_not():
    small = encode_apply_op_frames({"op": "set", "module": "M", "control": "c", "value": 1})
    assert len(small) == 1
    payload = small[0][9:-1]          # envelope: 'IMPROV'(6) ver(1) type(1) len(1) ... csum(1)
    assert payload[0] == IMPROV_CMD_APPLY_OP and payload[1] == 0 and payload[2] == 1
    big = {"op": "set", "module": "RmtLed", "control": "pins", "value": ",".join(str(i) for i in range(120))}
    frames = encode_apply_op_frames(big)
    assert len(frames) > 1
    seqs, lasts, body = [], [], b""
    for f in frames:
        pl = f[9:-1]
        assert pl[0] == IMPROV_CMD_APPLY_OP
        assert len(pl) - 3 <= APPLY_OP_CHUNK_MAX
        seqs.append(pl[1]); lasts.append(pl[2]); body += pl[3:]
    assert seqs == list(range(len(frames)))
    assert lasts == [0] * (len(frames) - 1) + [1]
    import json
    assert json.loads(body) == big


def test_the_script_no_longer_sends_a_vendor_rpc_the_firmware_does_not_have():
    src = (Path(__file__).resolve().parents[2] / "moondeck" / "build" / "improv_provision.py").read_text()
    assert "0xFE" not in src and "SET_DEVICE_MODEL" not in src


# --- the closed-loop sender, against a scripted fake port ---------------------------------

from improv_provision import (send_apply_op, build_frame, TYPE_CURRENT_STATE, TYPE_ERROR_STATE,
                              TYPE_RPC_RESPONSE)


class FakePort:
    """A serial port whose replies are scripted: each write() queues the next reply frame."""
    def __init__(self, replies):
        self.replies = list(replies)      # one entry per write: a list of frames the device answers with
        self.rx = bytearray()
        self.writes = 0
    def write(self, data): 
        self.writes += 1
        for frame in (self.replies.pop(0) if self.replies else []):
            self.rx += frame
    def flush(self): pass
    def reset_input_buffer(self): self.rx.clear()
    def read(self, n):
        out = bytes(self.rx[:n]); del self.rx[:n]; return out


OP = {"op": "set", "module": "M", "control": "c", "value": 1}


def test_a_state_frame_arriving_before_the_ack_is_chatter_not_a_refusal():
    # Right after provisioning the device still announces PROVISIONED on its own; that frame
    # arrived first and was read as the verdict, so the very first op of every push failed.
    port = FakePort([[build_frame(TYPE_CURRENT_STATE, b"\x04"), build_frame(TYPE_RPC_RESPONSE, b"")]])
    assert send_apply_op(port, OP, ack_timeout=1.0) is True
    assert port.writes == 1


def test_a_busy_device_gets_the_same_frame_again_until_it_acks():
    busy = build_frame(TYPE_ERROR_STATE, b"\x82")
    port = FakePort([[busy], [busy], [build_frame(TYPE_RPC_RESPONSE, b"")]])
    assert send_apply_op(port, OP, ack_timeout=1.0) is True
    assert port.writes == 3


def test_any_other_error_is_a_refusal_and_the_op_fails_once():
    port = FakePort([[build_frame(TYPE_ERROR_STATE, b"\x03")]])   # unknown RPC
    assert send_apply_op(port, OP, ack_timeout=1.0) is False
    assert port.writes == 1


def test_no_ack_at_all_fails_instead_of_claiming_success():
    port = FakePort([[]])
    assert send_apply_op(port, OP, ack_timeout=0.2) is False


from improv_provision import is_eth_only


def test_an_ethernet_only_entry_is_recognized_the_same_way_the_browser_does_it():
    # install.js: ethOnly = /-eth$/.test(firmware). `-eth-wifi` is the co-processor build and is NOT eth-only.
    assert is_eth_only({"firmwares": ["esp32p4rev1-eth"]})
    assert not is_eth_only({"firmwares": ["esp32p4rev1-eth-wifi"]})
    assert not is_eth_only({"firmwares": ["esp32"]})
    assert not is_eth_only({"firmwares": []}) and not is_eth_only({}) and not is_eth_only(None)
