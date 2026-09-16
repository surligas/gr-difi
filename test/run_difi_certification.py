#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# Copyright (C) 2026 Libre Space Foundation <https://libre.space>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

import argparse
import glob
import os
import signal
import socket
import subprocess
import sys
import time


def get_free_udp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("", 0))
        return s.getsockname()[1]


def cleanup_temp_files(directory):
    patterns = [
        "temp.pcap",
        "certify_source_summary_*.yaml",
        "certify_sink_summary_*.yaml",
        "power_spectral_density.png",
        "iq_recording_spectrogram.png",
        "iq_recording.sigmf-*",
        "error_log.txt",
    ]
    for pat in patterns:
        for f in glob.glob(os.path.join(directory, pat)):
            try:
                os.remove(f)
            except OSError:
                pass


def main():
    parser = argparse.ArgumentParser(description="Run DIFI certification integration tests")
    parser.add_argument("--cert-dir", required=True, help="Path to DIFI-Certification repository")
    parser.add_argument("--tx-bin", required=True, help="Path to difi_cert_tx executable")
    parser.add_argument("--rx-bin", required=True, help="Path to difi_cert_rx executable")
    parser.add_argument("--venv", default="", help="Path to Python virtual environment")
    args = parser.parse_args()

    # Re-exec under virtual environment if available and not already inside it
    if args.venv:
        venv_python = os.path.abspath(os.path.join(args.venv, "bin", "python"))
        if os.path.exists(venv_python) and os.path.realpath(sys.executable) != os.path.realpath(venv_python):
            print(f"[run_difi_certification] Re-executing with venv python: {venv_python}")
            os.execv(venv_python, [venv_python] + sys.argv)

    # Check required Python modules
    required_modules = ["scapy", "numpy", "matplotlib", "yaml", "construct"]
    missing = []
    for mod in required_modules:
        try:
            __import__(mod)
        except ImportError:
            missing.append(mod)

    if missing:
        print(f"[run_difi_certification] Skipping test: missing Python dependencies ({', '.join(missing)})")
        sys.exit(77)  # Standard CTest skip return code

    cert_source_script = os.path.join(args.cert_dir, "certify_source.py")
    cert_sink_script = os.path.join(args.cert_dir, "certify_sink.py")

    if not os.path.exists(cert_source_script) or not os.path.exists(cert_sink_script):
        print(f"[run_difi_certification] Skipping test: certification scripts not found in {args.cert_dir}")
        sys.exit(77)

    env = os.environ.copy()
    env["MPLBACKEND"] = "Agg"
    cert_dir_abs = os.path.abspath(args.cert_dir)
    existing_pythonpath = env.get("PYTHONPATH", "")
    env["PYTHONPATH"] = cert_dir_abs if not existing_pythonpath else f"{cert_dir_abs}:{existing_pythonpath}"

    work_dir = os.getcwd()
    cleanup_temp_files(work_dir)

    print("\n=======================================================")
    print("Test 1: Validate C++ Transmitter (difi_cert_tx) -> certify_source.py")
    print("=======================================================")
    port1 = get_free_udp_port()
    error_log = os.path.join(work_dir, "error_log.txt")

    source_proc = subprocess.Popen(
        [
            sys.executable,
            "-u",
            cert_source_script,
            "--udp-port",
            str(port1),
            "--difi-version",
            "1.2.1",
            "--error-log",
            error_log,
        ],
        cwd=work_dir,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    # Wait for certify_source.py to start listening on the UDP port
    start_wait = time.time()
    while time.time() - start_wait < 10.0:
        line = source_proc.stdout.readline()
        if line:
            print(f"[certify_source] {line.strip()}")
            if "Recording UDP packets on port" in line:
                break
        if source_proc.poll() is not None:
            break
        time.sleep(0.05)

    try:
        tx_res = subprocess.run(
            [args.tx_bin, "--port", str(port1), "--bit-depth", "16", "--num-packets", "40"],
            check=True,
            timeout=10,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        print(tx_res.stdout.strip())
    except Exception as e:
        source_proc.kill()
        print(f"[run_difi_certification] Error running difi_cert_tx: {e}")
        sys.exit(1)

    # Allow time for datagrams to be read by the socket before interrupting
    time.sleep(0.5)
    source_proc.send_signal(signal.SIGINT)

    try:
        stdout, stderr = source_proc.communicate(timeout=15)
        print(f"[run_difi_certification] certify_source returncode: {source_proc.returncode}")
        if stdout:
            print(f"[run_difi_certification] certify_source stdout:\n{stdout}")
        if stderr:
            print(f"[run_difi_certification] certify_source stderr:\n{stderr}")
    except subprocess.TimeoutExpired:
        source_proc.kill()
        print("[run_difi_certification] certify_source timed out after SIGINT")
        sys.exit(1)

    # Check generated summary YAML
    summaries = glob.glob(os.path.join(work_dir, "certify_source_summary_*.yaml"))
    if not summaries:
        print("[run_difi_certification] No certify_source_summary_*.yaml generated!")
        if os.path.exists(error_log):
            with open(error_log, "r") as ef:
                print(f"Error log contents:\n{ef.read()}")
        sys.exit(1)

    import yaml

    with open(summaries[0], "r") as f:
        summary_data = yaml.safe_load(f)

    print(f"[run_difi_certification] Summary Report: {summary_data}")
    overall = summary_data.get("overall_result")
    if overall != "PASS":
        print(f"[run_difi_certification] Test 1 FAILED! Overall result: {overall}")
        if os.path.exists(error_log):
            with open(error_log, "r") as ef:
                print(f"Error log contents:\n{ef.read()}")
        sys.exit(1)

    print("[run_difi_certification] Test 1 PASSED: DIFI frames verified compliant by certify_source.py")

    print("\n=======================================================")
    print("Test 2: Validate certify_sink.py -> C++ Receiver (difi_cert_rx)")
    print("=======================================================")
    port2 = get_free_udp_port()

    rx_proc = subprocess.Popen(
        [
            args.rx_bin,
            "--port",
            str(port2),
            "--expected-bit-depth",
            "16",
            "--min-packets",
            "20",
            "--timeout-ms",
            "6000",
        ],
        cwd=work_dir,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    # Wait for difi_cert_rx to bind and listen
    start_wait = time.time()
    while time.time() - start_wait < 5.0:
        line = rx_proc.stdout.readline()
        if line:
            print(f"[difi_cert_rx] {line.strip()}")
            if "Listening on port" in line:
                break
        if rx_proc.poll() is not None:
            break
        time.sleep(0.05)

    try:
        sink_res = subprocess.run(
            [
                sys.executable,
                "-u",
                cert_sink_script,
                "--port",
                str(port2),
                "--duration",
                "1.5",
                "--bit-depth",
                "16",
                "--sample-rate",
                "100e3",
                "--packet-size",
                "small",
                "--difi-version",
                "1.2.1",
            ],
            cwd=work_dir,
            env=env,
            check=True,
            timeout=10,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        print(sink_res.stdout.strip())
    except Exception as e:
        rx_proc.kill()
        print(f"[run_difi_certification] Error running certify_sink: {e}")
        sys.exit(1)

    try:
        rx_stdout, rx_stderr = rx_proc.communicate(timeout=6)
        print(rx_stdout)
        if rx_proc.returncode != 0:
            print(f"[run_difi_certification] difi_cert_rx failed with code {rx_proc.returncode}")
            print(f"rx_stderr: {rx_stderr}")
            sys.exit(1)
    except subprocess.TimeoutExpired:
        rx_proc.kill()
        print("[run_difi_certification] difi_cert_rx timed out")
        sys.exit(1)

    print("[run_difi_certification] Test 2 PASSED: certify_sink frames parsed and validated by difi_cert_rx")

    # Cleanup artifacts
    cleanup_temp_files(work_dir)

    print("\n=======================================================")
    print("[run_difi_certification] All DIFI certification integration tests PASSED!")
    print("=======================================================\n")
    sys.exit(0)


if __name__ == "__main__":
    main()
