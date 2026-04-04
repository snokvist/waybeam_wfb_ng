"""Tests for CLI argument parsing and subcommand routing."""

import random
import subprocess
import sys
import pytest


class TestCliTable:
    """Table subcommand prints reference table."""

    def test_table_subcommand(self):
        result = subprocess.run(
            [sys.executable, "-m", "fec_controller", "table"],
            capture_output=True, text=True, timeout=10,
        )
        assert result.returncode == 0
        assert "Reference table" in result.stdout
        assert "FrameSize" in result.stdout

    def test_no_args_prints_table_and_simulation(self):
        """No subcommand: prints table then runs default simulation."""
        result = subprocess.run(
            [sys.executable, "-m", "fec_controller"],
            capture_output=True, text=True, timeout=10,
        )
        assert result.returncode == 0
        assert "Reference table" in result.stdout
        assert "Simulating" in result.stdout


class TestCliSimulate:
    """Simulate subcommand runs synthetic stream."""

    def test_simulate_default(self):
        result = subprocess.run(
            [sys.executable, "-m", "fec_controller", "simulate"],
            capture_output=True, text=True, timeout=10,
        )
        assert result.returncode == 0
        assert "Simulating" in result.stdout

    def test_simulate_custom_args(self):
        result = subprocess.run(
            [sys.executable, "-m", "fec_controller", "simulate",
             "--fps", "60", "--base-frame-size", "3000", "--duration", "1.0"],
            capture_output=True, text=True, timeout=10,
        )
        assert result.returncode == 0
        assert "60fps" in result.stdout


class TestCliRun:
    """Run subcommand requires --wfb-port."""

    def test_run_missing_required_arg(self):
        """run without --wfb-port should fail."""
        result = subprocess.run(
            [sys.executable, "-m", "fec_controller", "run"],
            capture_output=True, text=True, timeout=10,
        )
        assert result.returncode != 0
        assert "wfb-port" in result.stderr
