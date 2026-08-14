#!/usr/bin/env python3
"""Isolated protocol/config test for the package-owned SLSsteam control helper."""
import json
import fcntl
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile


def send(peer, message):
    payload = json.dumps(message).encode()
    peer.sendall(struct.pack("<I", len(payload)) + payload)


def receive(peer):
    prefix = peer.recv(4)
    assert len(prefix) == 4
    remaining = struct.unpack("<I", prefix)[0]
    payload = b""
    while len(payload) < remaining:
        payload += peer.recv(remaining - len(payload))
    return json.loads(payload)


with tempfile.TemporaryDirectory(prefix="slssteam-control-") as temporary:
    home = Path(temporary)
    config_dir = home / ".config/SLSsteam"
    config_dir.mkdir(parents=True)
    config = config_dir / "config.yaml"
    config.write_text("PlayNotOwnedGames: yes\nUnknownFutureKey: keep-me\n")
    config.chmod(0o600)
    socket_path = home / "control.sock"
    listener = socket.socket(socket.AF_UNIX)
    listener.bind(str(socket_path))
    listener.listen(1)
    evidence_path = home / f".slssteam-ronin.ready.{os.getpid()}"
    evidence_file = evidence_path.open("w+")
    json.dump({"schema_version": "1", "steamclient_sha256": "a" * 64},
              evidence_file)
    evidence_file.flush()
    fcntl.flock(evidence_file, fcntl.LOCK_EX | fcntl.LOCK_NB)
    env = os.environ.copy()
    env.update({
        "HOME": str(home),
        "TSUKI_RONIN_SETTINGS_PATH": str(config),
        "TSUKI_RONIN_DATA_DIR": str(home / "managed-data"),
        "TSUKI_RONIN_RUNTIME_DIR": str(home),
        "TSUKI_RONIN_SOCKET": str(socket_path),
        "TSUKI_RONIN_MODULE_ID": "slsteam",
        "TSUKI_RONIN_COMPONENT_ID": "slssteam-control",
    })
    control_binary = os.environ.get(
        "SLSSTEAM_CONTROL_BIN", "bin/slssteam-control"
    )
    child = subprocess.Popen([control_binary], env=env)
    peer, _ = listener.accept()
    try:
        send(peer, {"v": 1, "t": "ctl", "id": "1", "method": "init", "payload": {}})
        assert receive(peer)["t"] == "res"
        send(peer, {"v": 1, "t": "ping", "id": "ping-1"})
        assert receive(peer) == {"v": 1, "t": "pong", "id": "ping-1"}
        send(peer, {
            "v": 1, "t": "req", "id": "settings-1",
            "method": "settings.get", "payload": {},
        })
        state = receive(peer)["payload"]
        assert state["values"]["PlayNotOwnedGames"] is True
        assert state["values"]["DisableCloud"] is True
        send(peer, {
            "v": 1, "t": "req", "id": "settings-2",
            "method": "settings.set",
            "payload": {
                "base_revision": state["revision"],
                "values": {
                    "DisableCloud": False,
                    "FakeEmail": "test@example.invalid",
                    "AchievementOwners": {"600": "76561198028121353"},
                },
            },
        })
        updated = receive(peer)["payload"]
        assert updated["revision"] != state["revision"]
        assert updated["values"]["DisableCloud"] is False
        assert updated["values"]["AchievementOwners"]["600"] == "76561198028121353"
        assert "UnknownFutureKey: keep-me" in config.read_text()
        send(peer, {
            "v": 1, "t": "req", "id": "settings-stale",
            "method": "settings.set",
            "payload": {
                "base_revision": state["revision"],
                "values": {"DisableCloud": True},
            },
        })
        assert "revision conflict" in receive(peer)["error"]["message"]
        for case_id, values, message in (
            ("settings-loglevel", {"LogLevel": 7}, "outside its declared range"),
            ("settings-wallet-negative", {"FakeWalletBalance": -1},
             "outside its declared range"),
            ("settings-wallet-overflow", {"FakeWalletBalance": 2147483648},
             "outside its declared range"),
            ("settings-schema-range", {"MaxSchemaTries": 1001},
             "outside its declared range"),
            ("settings-uint64", {
                "AchievementOwnerId": "18446744073709551616"
            }, "invalid AchievementOwnerId"),
        ):
            send(peer, {
                "v": 1, "t": "req", "id": case_id,
                "method": "settings.set",
                "payload": {
                    "base_revision": updated["revision"],
                    "values": values,
                },
            })
            range_error = receive(peer)
            assert range_error["t"] == "err", range_error
            assert message in range_error["error"]["message"]
        send(peer, {
            "v": 1, "t": "req", "id": "health-1",
            "method": "health.evidence.get", "payload": {},
        })
        evidence = receive(peer)["payload"]
        assert evidence["status"] == "ready"
        assert evidence["compatibility_value"] == "sha256:" + "a" * 64
        assert evidence["process_instance"].startswith(f"{os.getpid()}:")
        send(peer, {
            "v": 1, "t": "req", "id": "2", "method": "pins.set",
            "payload": {
                "appid": "600", "build_id": "42", "locked": True,
                "depots": {"800": "18446744073709551615"},
            },
        })
        result = receive(peer)
        assert result["payload"]["apps"][0]["depots"]["800"] == "18446744073709551615"
        text = config.read_text()
        assert "UnknownFutureKey: keep-me" in text
        assert '800: "18446744073709551615"' in text
        assert config.stat().st_mode & 0o777 == 0o600
        send(peer, {"v": 1, "t": "req", "id": "3", "method": "pins.list", "payload": {}})
        assert receive(peer)["payload"]["apps"][0]["build_id"] == "42"
        send(peer, {
            "v": 1, "t": "req", "id": "4", "method": "pins.clear",
            "payload": {"appid": "600"},
        })
        assert receive(peer)["payload"]["apps"] == []
        assert "ManifestPins:" not in config.read_text()

        history = {
            "appid": "600",
            "anchor": {
                "build_id": "10",
                "depots": {
                    "800": "300", "801": "400", "802": "500",
                    "999": "777",
                },
            },
            "builds": [
                {
                    "build_id": "10",
                    "transitions": {
                        "800": {"old": "200", "new": "300"},
                        "802": {"old": "0", "new": "500"},
                    },
                },
                {
                    "build_id": "9",
                    "transitions": {"801": {"old": "350", "new": "400"}},
                },
                {
                    "build_id": "8",
                    "transitions": {},
                },
            ],
        }
        send(peer, {
            "v": 1, "t": "req", "id": "5",
            "method": "pins.history.import", "payload": history,
        })
        imported = receive(peer)
        assert imported["t"] == "res"
        assert [item["build_id"] for item in imported["payload"]["builds"]] == ["10", "9", "8"]
        assert imported["payload"]["builds"][1]["depots"] == {"800": "200", "801": "400"}
        assert all("999" not in item["depots"] for item in imported["payload"]["builds"])
        assert imported["payload"]["builds"][2]["depots"] == {"800": "200", "801": "350"}
        cache = home / "managed-data/manifest-history/600.json"
        before_bad_import = cache.read_bytes()
        assert cache.stat().st_mode & 0o777 == 0o600

        broken = json.loads(json.dumps(history))
        broken["builds"][0]["transitions"]["800"]["new"] = "999"
        send(peer, {
            "v": 1, "t": "req", "id": "6",
            "method": "pins.history.import", "payload": broken,
        })
        error = receive(peer)
        assert error["t"] == "err"
        assert "does not join" in error["error"]["message"]
        assert cache.read_bytes() == before_bad_import

        send(peer, {
            "v": 1, "t": "req", "id": "7",
            "method": "pins.history.resolve",
            "payload": {"appid": "600", "build_id": "9"},
        })
        resolved = receive(peer)
        assert resolved["payload"] == {
            "appid": "600", "build_id": "9",
            "depots": {"800": "200", "801": "400"},
        }
        send(peer, {
            "v": 1, "t": "req", "id": "8",
            "method": "pins.history.list", "payload": {"appid": "600"},
        })
        assert receive(peer)["payload"]["builds"][0]["build_id"] == "10"

        composite = {
            "appid": "600",
            "anchor": {
                "build_id": "10",
                "depots": {
                    "800": "300", "900": "900", "902": "990",
                    "999": "777",
                },
            },
            "apps": [
                {
                    "appid": "600", "relation": "root",
                    "downloadable": True,
                    "depots": [
                        {"depot_id": "800", "owner_appid": "600",
                         "configuration": "Windows",
                         "size": {"bytes": "3330383071", "display": "3.10 GiB"},
                         "dl": {"bytes": "1652897536", "display": "1.54 GiB"}},
                        {"depot_id": "900", "owner_appid": "700",
                         "configuration": "DLC 700"},
                        {"depot_id": "910", "owner_appid": "228980",
                         "configuration": "Shared redistributable"},
                    ],
                    "branches": [
                        {"name": "public", "build_id": "10",
                         "built_at": "2026-02-01T00:00:00.000Z",
                         "updated_at": "2026-02-01T00:01:00.000Z"},
                    ],
                    "builds": [
                        {
                            "build_id": "10", "published_at": "2026-02-01T00:00:00.000Z",
                            "transitions": {"800": {"old": "200", "new": "300"}},
                        },
                        {
                            "build_id": "9", "published_at": "2026-01-01T00:00:00.000Z",
                            "transitions": {},
                        },
                    ],
                },
                {
                    "appid": "700", "relation": "dlc",
                    "downloadable": True,
                    "depots": [
                        {"depot_id": "900", "owner_appid": "700",
                         "configuration": "DLC payload"},
                    ],
                    "branches": [
                        {"name": "public", "build_id": "20",
                         "built_at": "2026-01-15T00:00:00.000Z",
                         "updated_at": "2026-01-15T00:01:00.000Z"},
                    ],
                    "builds": [
                        {
                            "build_id": "20", "published_at": "2026-01-15T00:00:00.000Z",
                            "transitions": {"900": {"old": "850", "new": "900"}},
                        },
                        {
                            "build_id": "19", "published_at": "2025-12-01T00:00:00.000Z",
                            "transitions": {},
                        },
                    ],
                },
                {
                    "appid": "701", "relation": "dlc",
                    "downloadable": True,
                    "depots": [
                        {"depot_id": "901", "owner_appid": "701",
                         "configuration": "Unused DLC"},
                    ],
                    "branches": [],
                    "builds": [
                        {
                            "build_id": "30", "published_at": "2026-01-20T00:00:00.000Z",
                            "transitions": {"901": {"old": "1", "new": "2"}},
                        },
                    ],
                },
                {
                    "appid": "702", "relation": "dlc",
                    "downloadable": True,
                    "depots": [
                        {"depot_id": "902", "owner_appid": "702",
                         "configuration": "Single-build DLC"},
                    ],
                    "branches": [
                        {"name": "public", "build_id": "40",
                         "built_at": "2026-01-10T00:00:00.000Z",
                         "updated_at": "2026-01-10T00:01:00.000Z"},
                    ],
                    "builds": [
                        {
                            "build_id": "40", "published_at": "2026-01-10T00:00:00.000Z",
                            "transitions": {},
                        },
                    ],
                },
            ],
            "shared_depot_histories": [
                {
                    "depot_id": "910", "owner_appid": "228980",
                    "category": "redistributable",
                    "manifests": [
                        {"manifest_id": "1100",
                         "seen_at": "2026-02-02T00:00:00.000Z"},
                        {"manifest_id": "1000",
                         "seen_at": "2026-01-15T00:00:00.000Z"},
                    ],
                },
            ],
            "availability_policy": {
                "dlc_release_between_base_builds": "preceding_base_build",
            },
        }
        send(peer, {
            "v": 1, "t": "req", "id": "9",
            "method": "pins.history.import", "payload": composite,
        })
        imported = receive(peer)
        assert imported["t"] == "res"
        assert [app["appid"] for app in imported["payload"]["apps"]] == ["600", "700", "702"]
        assert imported["payload"]["schema_version"] == "2.1"
        assert imported["payload"]["shared_depot_histories"][0]["depot_id"] == "910"
        send(peer, {
            "v": 1, "t": "req", "id": "10",
            "method": "pins.history.resolve",
            "payload": {"appid": "600", "build_id": "10"},
        })
        assert receive(peer)["payload"]["depots"] == {
            "800": "300", "900": "900", "902": "990",
        }
        send(peer, {
            "v": 1, "t": "req", "id": "11",
            "method": "pins.history.resolve",
            "payload": {"appid": "600", "build_id": "9"},
        })
        assert receive(peer)["payload"]["depots"] == {
            "800": "200", "900": "900", "902": "990",
        }

        send(peer, {
            "v": 1, "t": "req", "id": "feature-status",
            "method": "feature.status", "payload": {},
        })
        features = receive(peer)["payload"]["features"]
        assert {item["id"] for item in features} == {
            "added-games", "manifest-pinning", "achievement-schemas",
            "compatibility-tools", "steamstub-ticket",
        }
        assert all(item["status"] == "available" for item in features)
        feature_events = [receive(peer) for _ in features]
        assert all(item["t"] == "evt" for item in feature_events)
        assert all(item["method"] == "feature.changed" for item in feature_events)
        assert {item["payload"]["id"] for item in feature_events} == {
            item["id"] for item in features
        }

        send(peer, {
            "v": 1, "t": "req", "id": "pack-inspect",
            "method": "manifest-pack.inspect",
            "payload": {"app_id": "600", "build_id": "9", "action": "install"},
        })
        inspected = receive(peer)["payload"]
        assert inspected["plan"] == {
            "action": "install", "app_id": "600", "build_id": "9",
            "depots": {"800": "200", "900": "900", "902": "990"},
        }
        authority = {
            "operation_id": "op-install", "confirmation_handle": "confirm-install",
            "plan_digest": inspected["plan_digest"], "grant_id": "grant-install",
            "resource_handle": "resource-install",
        }
        send(peer, {
            "v": 1, "t": "req", "id": "pack-tampered",
            "method": "manifest-pack.install",
            "payload": {
                "app_id": "600", "plan": inspected["plan"],
                "plan_digest": "0" * 64, "user_gesture": True,
                "authority": authority,
            },
        })
        assert "plan changed" in receive(peer)["error"]["message"]
        send(peer, {
            "v": 1, "t": "req", "id": "pack-install",
            "method": "manifest-pack.install",
            "payload": {
                "app_id": "600", "plan": inspected["plan"],
                "plan_digest": inspected["plan_digest"], "user_gesture": True,
                "authority": authority,
            },
        })
        installed = receive(peer)["payload"]
        assert installed["verified"] is True
        assert installed["depots"] == inspected["plan"]["depots"]
        finalized_authority = dict(authority, phase="finalize")
        send(peer, {
            "v": 1, "t": "req", "id": "pack-install-finalize",
            "method": "manifest-pack.install",
            "payload": {
                "app_id": "600", "plan": inspected["plan"],
                "plan_digest": inspected["plan_digest"], "user_gesture": True,
                "authority": finalized_authority,
            },
        })
        assert receive(peer)["payload"]["verified"] is True
        assert receive(peer)["method"] == "manifest-pack.changed"
        send(peer, {
            "v": 1, "t": "req", "id": "pack-status",
            "method": "manifest-pack.status", "payload": {"app_id": "600"},
        })
        assert receive(peer)["payload"]["build_id"] == "9"

        send(peer, {
            "v": 1, "t": "req", "id": "pack-remove-inspect",
            "method": "manifest-pack.inspect",
            "payload": {"app_id": "600", "action": "remove"},
        })
        removal = receive(peer)["payload"]
        remove_authority = {
            "operation_id": "op-remove-rollback",
            "confirmation_handle": "confirm-remove-rollback",
            "plan_digest": removal["plan_digest"], "grant_id": "grant-remove",
            "resource_handle": "resource-remove",
        }
        remove_payload = {
            "app_id": "600", "plan": removal["plan"],
            "plan_digest": removal["plan_digest"], "user_gesture": True,
            "authority": remove_authority,
        }
        send(peer, {
            "v": 1, "t": "req", "id": "pack-remove-rollback-commit",
            "method": "manifest-pack.remove", "payload": remove_payload,
        })
        assert receive(peer)["payload"]["verified"] is True
        rollback_payload = dict(remove_payload)
        rollback_payload["authority"] = dict(remove_authority, phase="rollback")
        send(peer, {
            "v": 1, "t": "req", "id": "pack-remove-rollback",
            "method": "manifest-pack.remove", "payload": rollback_payload,
        })
        assert receive(peer)["payload"]["verified"] is True
        send(peer, {
            "v": 1, "t": "req", "id": "pack-status-after-rollback",
            "method": "manifest-pack.status", "payload": {"app_id": "600"},
        })
        assert receive(peer)["payload"]["build_id"] == "9"

        remove_authority = dict(
            remove_authority,
            operation_id="op-remove", confirmation_handle="confirm-remove",
        )
        send(peer, {
            "v": 1, "t": "req", "id": "pack-remove",
            "method": "manifest-pack.remove",
            "payload": {
                "app_id": "600", "plan": removal["plan"],
                "plan_digest": removal["plan_digest"], "user_gesture": True,
                "authority": remove_authority,
            },
        })
        removed = receive(peer)["payload"]
        assert removed["verified"] is True
        assert removed["action"] == "remove"
        remove_finalize = dict(remove_authority, phase="finalize")
        send(peer, {
            "v": 1, "t": "req", "id": "pack-remove-finalize",
            "method": "manifest-pack.remove",
            "payload": {
                "app_id": "600", "plan": removal["plan"],
                "plan_digest": removal["plan_digest"], "user_gesture": True,
                "authority": remove_finalize,
            },
        })
        assert receive(peer)["payload"]["verified"] is True
        assert receive(peer)["method"] == "manifest-pack.changed"
        send(peer, {
            "v": 1, "t": "req", "id": "pack-status-removed",
            "method": "manifest-pack.status", "payload": {"app_id": "600"},
        })
        assert receive(peer)["payload"] == {
            "app_id": "600", "installed": False, "depots": {},
        }

        valid_composite_cache = cache.read_bytes()
        incomplete_shared_history = json.loads(json.dumps(composite))
        incomplete_shared_history["shared_depot_histories"][0]["manifests"] = [
            {"manifest_id": "1200", "seen_at": "2026-03-01T00:00:00.000Z"},
            {"manifest_id": "1100", "seen_at": "2026-02-02T00:00:00.000Z"},
        ]
        send(peer, {
            "v": 1, "t": "req", "id": "12", "method": "pins.history.import",
            "payload": incomplete_shared_history,
        })
        rejected = receive(peer)
        assert rejected["t"] == "err"
        assert "does not cover every root build interval" in rejected["error"]["message"]
        assert cache.read_bytes() == valid_composite_cache

        send(peer, {"v": 1, "t": "ctl", "id": "13", "method": "shutdown", "payload": {}})
        assert receive(peer)["t"] == "res"
    finally:
        peer.close()
        listener.close()
        evidence_file.close()
    assert child.wait(timeout=3) == 0

print("test_slssteam_control: ALL PASS")
