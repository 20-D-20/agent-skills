from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "workflow_state.py"


class WorkflowStateTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.repo = Path(self.temp_dir.name) / "repo"
        self.repo.mkdir()
        self.git("init")
        self.git("config", "user.name", "Staged Dev Test")
        self.git("config", "user.email", "staged-dev@example.invalid")
        (self.repo / "base.txt").write_text("base\n", encoding="utf-8")
        self.git("add", "base.txt")
        self.git("commit", "-m", "initial")

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def git(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["git", "-C", str(self.repo), *args],
            text=True,
            encoding="utf-8",
            errors="replace",
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=True,
        )

    def workflow(
        self, *args: str, expect_success: bool = True
    ) -> subprocess.CompletedProcess[str]:
        result = subprocess.run(
            [sys.executable, "-X", "utf8", str(SCRIPT), *args, "--repo", str(self.repo)],
            text=True,
            encoding="utf-8",
            errors="replace",
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if expect_success and result.returncode != 0:
            self.fail(f"workflow failed: stdout={result.stdout!r} stderr={result.stderr!r}")
        if not expect_success and result.returncode == 0:
            self.fail(f"workflow unexpectedly succeeded: stdout={result.stdout!r}")
        return result

    def task_dir(self, task_id: str) -> Path:
        return self.repo / ".codex" / "tasks" / task_id

    def init_and_fill_plan(self, task_id: str) -> None:
        self.workflow("init", "--task-id", task_id, "--title", task_id)
        (self.task_dir(task_id) / "PLAN.md").write_text(
            "# Plan\n\n## 待确认问题 / Open Questions\n\n- 无。\n",
            encoding="utf-8",
        )

    def approve(self, task_id: str) -> None:
        self.workflow("approve", "--task-id", task_id, "--user-approved")

    def fill_implementation(self, task_id: str, cycle: int) -> None:
        path = self.task_dir(task_id) / "implementations" / f"implementation-{cycle:03d}.md"
        path.write_text("# Implementation complete\n", encoding="utf-8")

    def fill_review(self, task_id: str, cycle: int, verdict: str) -> None:
        path = self.task_dir(task_id) / "reviews" / f"review-{cycle:03d}.md"
        path.write_text(
            f"# Review\n\n## 结论 / Verdict\n\n{verdict}\n",
            encoding="utf-8",
        )

    def state(self, task_id: str) -> dict:
        result = self.workflow("status", "--task-id", task_id)
        return json.loads(result.stdout)

    def test_normal_flow_resume_clean_baseline_and_snapshot_drift(self) -> None:
        task_id = "normal-flow"
        self.init_and_fill_plan(task_id)

        (self.repo / "base.txt").write_text("unexpected pre-approval edit\n", encoding="utf-8")
        failed = self.workflow(
            "approve", "--task-id", task_id, "--user-approved", expect_success=False
        )
        self.assertIn("clean non-task working tree", failed.stderr)
        self.git("checkout", "--", "base.txt")
        self.approve(task_id)

        self.workflow("begin-implementation", "--task-id", task_id)
        resumed = self.workflow("begin-implementation", "--task-id", task_id)
        self.assertIn("resuming implementation cycle 001", resumed.stdout)

        (self.repo / "base.txt").write_text("implemented\n", encoding="utf-8")
        (self.repo / "new-source.c").write_text("int value = 1;\n", encoding="utf-8")
        self.fill_implementation(task_id, 1)
        completed = self.workflow("complete-implementation", "--task-id", task_id)
        self.assertIn("snapshot=", completed.stdout)

        self.workflow("begin-review", "--task-id", task_id)
        resumed_review = self.workflow("begin-review", "--task-id", task_id)
        self.assertIn("resuming review cycle 001", resumed_review.stdout)
        self.fill_review(task_id, 1, "PASS")

        (self.repo / "new-source.c").write_text("int value = 2;\n", encoding="utf-8")
        drift = self.workflow(
            "complete-review", "--task-id", task_id, "--verdict", "pass", expect_success=False
        )
        self.assertIn("snapshot drift detected", drift.stderr)
        (self.repo / "new-source.c").write_text("int value = 1;\n", encoding="utf-8")

        self.workflow("complete-review", "--task-id", task_id, "--verdict", "pass")
        self.assertEqual("passed", self.state(task_id)["stage"])

    def test_changes_requested_starts_next_cycle_from_reviewed_snapshot(self) -> None:
        task_id = "rework-flow"
        self.init_and_fill_plan(task_id)
        self.approve(task_id)
        self.workflow("begin-implementation", "--task-id", task_id)
        (self.repo / "base.txt").write_text("cycle one\n", encoding="utf-8")
        self.fill_implementation(task_id, 1)
        self.workflow("complete-implementation", "--task-id", task_id)
        self.workflow("begin-review", "--task-id", task_id)
        self.fill_review(task_id, 1, "CHANGES_REQUESTED")
        self.workflow(
            "complete-review", "--task-id", task_id, "--verdict", "changes_requested"
        )

        self.workflow("begin-implementation", "--task-id", task_id)
        state = self.state(task_id)
        self.assertEqual("implementing", state["stage"])
        self.assertEqual(2, state["cycle"])

    def test_block_escapes_active_stage_and_allows_plan_revision(self) -> None:
        task_id = "blocked-flow"
        self.init_and_fill_plan(task_id)
        self.approve(task_id)
        self.workflow("begin-implementation", "--task-id", task_id)
        self.workflow(
            "block", "--task-id", task_id, "--reason", "approved plan conflicts with code"
        )
        state = self.state(task_id)
        self.assertEqual("blocked", state["stage"])
        self.assertEqual("implementing", state["blocked_from"])

        self.workflow("revise-plan", "--task-id", task_id)
        state = self.state(task_id)
        self.assertEqual("draft", state["stage"])
        self.assertEqual(2, state["plan_version"])

    def test_reviewer_can_block_without_completing_report(self) -> None:
        task_id = "review-blocked"
        self.init_and_fill_plan(task_id)
        self.approve(task_id)
        self.workflow("begin-implementation", "--task-id", task_id)
        (self.repo / "base.txt").write_text("ready for review\n", encoding="utf-8")
        self.fill_implementation(task_id, 1)
        self.workflow("complete-implementation", "--task-id", task_id)
        self.workflow("begin-review", "--task-id", task_id)

        self.workflow(
            "block", "--task-id", task_id, "--reason", "review environment unavailable"
        )
        state = self.state(task_id)
        self.assertEqual("blocked", state["stage"])
        self.assertEqual("reviewing", state["blocked_from"])
        self.assertEqual("review environment unavailable", state["blocked_reason"])

    def test_head_drift_blocks_implementation(self) -> None:
        task_id = "head-drift"
        self.init_and_fill_plan(task_id)
        self.approve(task_id)
        self.git("commit", "--allow-empty", "-m", "unexpected head change")
        failed = self.workflow(
            "begin-implementation", "--task-id", task_id, expect_success=False
        )
        self.assertIn("baseline drift detected", failed.stderr)


if __name__ == "__main__":
    unittest.main()
