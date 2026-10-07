#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Normalize legacy Sunshine aliases in the private, copied pairing profile."""

import json
import pathlib
import ssl


def normalize_pairing_state(path: pathlib.Path) -> int:
    """Retain the first record for an otherwise identical certificate identity.

    Sunshine can save several names/UUIDs for the same client certificate.
    Vibepollo requires a single unambiguous authorization record per certificate.
    Conflicting permissions or configuration must never be silently discarded.
    The caller must pass a private copy, leaving the source and backup intact.
    """
    state = json.loads(path.read_text())
    devices = state.get("root", {}).get("named_devices")
    if devices is None:
        return 0
    if not isinstance(devices, list):
        raise ValueError("pairing state named_devices is not an array")
    retained = []
    identities = {}
    for device in devices:
        if not isinstance(device, dict):
            raise ValueError("pairing state has a malformed client record")
        try:
            identity = ssl.PEM_cert_to_DER_cert(device["cert"])
        except (KeyError, TypeError, ValueError) as error:
            raise ValueError("pairing state has an invalid client certificate") from error
        # Name and UUID are legacy aliases. The exact certificate establishes
        # the client identity; every other field can affect its authorization
        # or streaming behavior and must agree before aliases are consolidated.
        settings = {key: value for key, value in device.items() if key not in {"name", "uuid", "cert"}}
        if identity in identities:
            if settings != identities[identity]:
                raise ValueError("duplicate client certificate has conflicting permissions or settings")
            continue
        identities[identity] = settings
        retained.append(device)
    removed = len(devices) - len(retained)
    if removed:
        state["root"]["named_devices"] = retained
        path.write_text(json.dumps(state, indent=2) + "\n")
    return removed
