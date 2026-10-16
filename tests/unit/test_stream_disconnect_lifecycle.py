#!/usr/bin/env python3
"""Exercise production disconnect bodies with deterministic, socket-free stubs.

Prepared for issue #510. --emit-only generates the C++ translation unit without
using a build slot. The unmodified baseline is expected to fail several cases.
The fixture models ENet's queue-discard contract, not network delivery or ACKs.
"""

import argparse
import pathlib
import re
import subprocess
import tempfile


def block(source, marker):
    """Extract one unambiguous C++ block, ignoring braces in strings/comments."""
    if source.count(marker) != 1:
        raise ValueError(f"Expected exactly one production marker: {marker!r}")
    start = source.index(marker)
    opening = source.index("{", start)
    scrubbed = re.sub(
        r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
        lambda match: " " * len(match[0]),
        source,
        flags=re.DOTALL,
    )
    depth = 0
    for end in range(opening, len(source)):
        depth += (scrubbed[end] == "{") - (scrubbed[end] == "}")
        if depth == 0:
            return source[start : end + 1]
    raise ValueError(f"Unbalanced production block: {marker!r}")


def program(root, revision=None):
    def read(path):
        if revision:
            return subprocess.check_output(["git", "-C", str(root), "show", f"{revision}:{path}"], text=True)
        return (root / path).read_text()

    stream = read("src/stream.cpp")
    header = read("src/stream.h")
    rtsp = read("src/rtsp.cpp")
    rtsp_header = read("src/rtsp.h")
    nvhttp = read("src/nvhttp.cpp")
    process = read("src/process.cpp")
    route = block(rtsp, "client_disconnect_result_t disconnect_client(")
    pause = block(process, "void proc_t::pause(bool stream_lifecycle_lock_held)")
    pause = pause[:pause.index('    BOOST_LOG(info) << "Session pausing for app')]
    pause += "    ++pause_continued;\n  }"
    termination_setup = stream[stream.index("    std::uint32_t termination_reason ="):
                               stream.index("    auto send_termination =")]
    snippets = {
        "STATE": block(header, "enum class state_e : int") + ";",
        "REASON": block(stream, "enum class stop_reason_e") + ";" if "enum class stop_reason_e" in stream else "",
        "REASON_FIELD": "std::atomic<stop_reason_e> stop_reason {stop_reason_e::none};" if "enum class stop_reason_e" in stream else "",
        "REQUEST_STOP": block(stream, "static void request_stop(session_t &session,") if "static void request_stop(" in stream else "",
        "WIRE_HEADER": block(stream, "struct control_header_v2") + ";",
        "WIRE_TERMINATION": block(stream, "struct control_terminate_t") + ";",
        "WIRE_ENCRYPTED": block(stream, "typedef struct control_encrypted_t") + " *control_encrypted_p;",
        "ENCODE": block(stream, "template<std::size_t max_payload_size>"),
        "PACKET_TYPES": block(stream, "static const short packetTypes[] =") + ";",
        "SEND_METHOD": block(stream, "int send(const std::string_view &payload, net::peer_t peer, bool haptics = false)"),
        "FLUSH_METHOD": block(stream, "void flush()"),
        "STOP": block(stream, "void stop(session_t &session)"),
        "GRACEFUL_STOP": block(stream, "void graceful_stop(session_t &session)"),
        "UPDATE_DEVICE": block(stream, "bool update_device_info(session_t &session,"),
        "SEND_TERMINATION": termination_setup + block(stream, "auto send_termination =") + ";",
        "STOPPING": block(stream, "if (session->state.load(std::memory_order_acquire) == session::state_e::STOPPING)"),
        "DISCONNECT": block(route, "for (auto &slot : to_cleanup)"),
        "NVHTTP_DISCONNECT": block(nvhttp, "bool disconnect_client(const std::string &uuid)"),
        "RTSP_DECLARATION": next(line.strip() for line in rtsp_header.splitlines()
                                 if "client_disconnect_result_t disconnect_client_sessions_with_result(" in line),
        "RTSP_WITH_RESULT": block(rtsp, "client_disconnect_result_t disconnect_client_sessions_with_result("),
        "RTSP_WITHOUT_RESULT": block(rtsp, "bool disconnect_client_sessions("),
        "PAUSE_GATE": block(stream, "if (!rtsp_pending && !webrtc_active)"),
        "PAUSE_ADMISSION": pause,
    }
    template = pathlib.Path(__file__).with_suffix(".cpp.in").read_text()
    for name, body in snippets.items():
        token = f"/* PRODUCTION_{name} */"
        if template.count(token) != 1:
            raise ValueError(f"Expected exactly one fixture token: {token}")
        template = template.replace(token, body)
    if "/* PRODUCTION_" in template:
        raise ValueError("Unreplaced fixture token")
    return template


CASES = (
    "devices_disconnect",
    "request_ownership",
    "graceful_delivery",
    "transport_failure",
    "failure_wins",
    "graceful_wins",
    "pending_peer",
    "send_failure",
    "publication_interleaving",
    "permission_revocation",
    "permission_update",
    "other_client_survives",
    "administrative_revoke",
    "encrypted_legacy",
    "encrypted_v2",
    "encryption_failure",
    "paused_application",
    "configured_termination",
    "racing_failure_wins",
    "racing_graceful_wins",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repository", nargs="?", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parents[2])
    parser.add_argument("--compiler", default="g++")
    parser.add_argument("--revision", help="Read production bodies from this local git revision")
    parser.add_argument("--emit-only", type=pathlib.Path,
                        help="Write generated source; do not compile or execute")
    args = parser.parse_args()
    generated = program(args.repository, args.revision)
    if args.emit_only:
        args.emit_only.write_text(generated)
        print(f"Prepared {len(CASES)} lifecycle cases in {args.emit_only}; not compiled or run")
        return
    with tempfile.TemporaryDirectory(prefix="disconnect-lifecycle-") as directory:
        source = pathlib.Path(directory) / "test.cpp"
        binary = pathlib.Path(directory) / "test"
        source.write_text(generated)
        subprocess.run([args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(source), "-o", str(binary)], check=True)
        failed = []
        for case in CASES:
            result = subprocess.run([str(binary), case], check=False, capture_output=True, text=True)
            print(f"{'FAIL' if result.returncode else 'PASS'} {case}: "
                  f"{(result.stdout + result.stderr).strip()}")
            if result.returncode:
                failed.append(case)
        if failed:
            raise SystemExit(f"{len(failed)} failed lifecycle cases: {', '.join(failed)}")


if __name__ == "__main__":
    main()
