#!/usr/bin/env python3
"""为 staged-dev skill 提供确定性的状态与文档管理。"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path


TASK_ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")
REQUIRED_MARKERS = ("<!-- 必填", "<!-- REQUIRED")
EXCLUDE_PATTERN = "/.codex/tasks/"


class WorkflowError(RuntimeError):
    pass


def now_iso() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat()


def run_git_bytes(repo: Path, *args: str) -> bytes:
    result = subprocess.run(
        ["git", "-C", str(repo), *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        error = result.stderr.decode("utf-8", errors="replace").strip()
        raise WorkflowError(error or f"git {' '.join(args)} failed")
    return result.stdout


def run_git(repo: Path, *args: str) -> str:
    return run_git_bytes(repo, *args).decode("utf-8", errors="replace").strip()


def repo_root(value: str) -> Path:
    candidate = Path(value).expanduser().resolve()
    root = run_git(candidate, "rev-parse", "--show-toplevel")
    return Path(root).resolve()


def validate_task_id(task_id: str) -> None:
    if not TASK_ID_RE.fullmatch(task_id):
        raise WorkflowError(
            "task-id must be 1-64 lowercase characters using letters, digits, '.', '_' or '-', and start with a letter or digit"
        )


def task_dir(repo: Path, task_id: str) -> Path:
    validate_task_id(task_id)
    return repo / ".codex" / "tasks" / task_id


def state_path(repo: Path, task_id: str) -> Path:
    return task_dir(repo, task_id) / "workflow.json"


def load_state(repo: Path, task_id: str) -> dict:
    path = state_path(repo, task_id)
    if not path.is_file():
        raise WorkflowError(f"task does not exist: {path}")
    return json.loads(path.read_text(encoding="utf-8"))


def save_state(repo: Path, task_id: str, state: dict) -> None:
    path = state_path(repo, task_id)
    state["schema_version"] = 2
    state["updated_at"] = now_iso()
    temp = path.with_suffix(".json.tmp")
    temp.write_text(json.dumps(state, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    temp.replace(path)


def template(name: str) -> str:
    path = Path(__file__).resolve().parent.parent / "assets" / "templates" / name
    return path.read_text(encoding="utf-8")


def render(name: str, **values: object) -> str:
    text = template(name)
    for key, value in values.items():
        text = text.replace("{{" + key.upper() + "}}", str(value))
    return text


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def ensure_local_exclude(repo: Path) -> None:
    raw = run_git(repo, "rev-parse", "--git-path", "info/exclude")
    exclude = Path(raw)
    if not exclude.is_absolute():
        exclude = repo / exclude
    exclude.parent.mkdir(parents=True, exist_ok=True)
    current = exclude.read_text(encoding="utf-8", errors="replace") if exclude.exists() else ""
    if EXCLUDE_PATTERN in {line.strip() for line in current.splitlines()}:
        return
    prefix = "" if not current or current.endswith(("\n", "\r")) else "\n"
    exclude.write_text(current + prefix + EXCLUDE_PATTERN + "\n", encoding="utf-8")


def git_snapshot(repo: Path) -> tuple[str, list[str]]:
    commit = run_git(repo, "rev-parse", "HEAD")
    raw = run_git(repo, "status", "--porcelain=v1", "--untracked-files=all")
    dirty = [line for line in raw.splitlines() if ".codex/tasks/" not in line.replace("\\", "/")]
    return commit, dirty


def working_tree_sha256(repo: Path) -> str:
    """计算 HEAD 之上的 tracked diff 与 non-ignored untracked 内容摘要。"""
    digest = hashlib.sha256()
    digest.update(b"tracked-diff\0")
    digest.update(
        run_git_bytes(
            repo,
            "diff",
            "--binary",
            "--no-ext-diff",
            "HEAD",
            "--",
            ".",
            ":(exclude).codex/tasks/**",
        )
    )

    raw_paths = run_git_bytes(repo, "ls-files", "--others", "--exclude-standard", "-z")
    for raw_path in sorted(path for path in raw_paths.split(b"\0") if path):
        relative = raw_path.decode("utf-8", errors="surrogateescape")
        if relative.replace("\\", "/").startswith(".codex/tasks/"):
            continue
        path = repo / relative
        if not path.exists() and not path.is_symlink():
            raise WorkflowError(f"untracked file disappeared while hashing: {relative}")
        content = (
            os.readlink(path).encode("utf-8", errors="surrogateescape")
            if path.is_symlink()
            else path.read_bytes()
        )
        digest.update(b"untracked\0")
        digest.update(raw_path)
        digest.update(b"\0")
        digest.update(hashlib.sha256(content).digest())
    return digest.hexdigest()


def require_stage(state: dict, *allowed: str) -> None:
    if state["stage"] not in allowed:
        raise WorkflowError(
            f"invalid transition from stage '{state['stage']}'; expected one of: {', '.join(allowed)}"
        )


def require_complete_artifact(path: Path) -> None:
    if not path.is_file():
        raise WorkflowError(f"required artifact is missing: {path}")
    text = path.read_text(encoding="utf-8")
    if any(marker in text for marker in REQUIRED_MARKERS):
        raise WorkflowError(f"文档中仍有必填标记：{path}")


def require_plan_hash(repo: Path, task_id: str, state: dict) -> None:
    plan = task_dir(repo, task_id) / "PLAN.md"
    expected = state.get("approved_plan_sha256")
    actual = sha256(plan)
    if not expected or actual != expected:
        raise WorkflowError("PLAN.md differs from the approved plan; return to planner and re-approve")


def require_baseline_commit(repo: Path, state: dict) -> None:
    commit = run_git(repo, "rev-parse", "HEAD")
    expected = state.get("baseline_commit")
    if not expected or commit != expected:
        raise WorkflowError(
            "Git HEAD differs from the approved baseline; do not reset or discard changes. "
            f"approved commit={expected} current commit={commit}"
        )


def require_baseline(repo: Path, state: dict) -> None:
    commit, dirty = git_snapshot(repo)
    if commit != state.get("baseline_commit") or dirty:
        raise WorkflowError(
            "Git baseline drift detected; implementation must start from the approved commit "
            "with a clean non-task working tree. Do not reset or discard changes automatically. "
            f"approved commit={state.get('baseline_commit')} current commit={commit}; "
            f"current dirty={dirty}"
        )


def require_implementation_snapshot(repo: Path, state: dict) -> None:
    require_baseline_commit(repo, state)
    expected = state.get("implementation_snapshot_sha256")
    if not expected:
        raise WorkflowError(
            "implementation snapshot is missing; this task may predate the current workflow. "
            "Do not infer a baseline from the live checkout"
        )
    actual = working_tree_sha256(repo)
    if actual != expected:
        raise WorkflowError(
            "implementation snapshot drift detected; stop and report the live Git changes. "
            f"expected={expected} current={actual}"
        )


def command_init(args: argparse.Namespace) -> None:
    repo = repo_root(args.repo)
    target = task_dir(repo, args.task_id)
    if target.exists():
        raise WorkflowError(f"task already exists: {target}")
    ensure_local_exclude(repo)
    target.mkdir(parents=True)
    (target / "implementations").mkdir()
    (target / "reviews").mkdir()
    (target / "plans").mkdir()
    (target / "PLAN.md").write_text(
        render("PLAN.md", task_id=args.task_id, title=args.title), encoding="utf-8"
    )
    state = {
        "schema_version": 2,
        "task_id": args.task_id,
        "title": args.title,
        "storage": "local",
        "stage": "draft",
        "cycle": 0,
        "plan_version": 1,
        "baseline_commit": None,
        "baseline_dirty_files": [],
        "approved_plan_sha256": None,
        "approved_at": None,
        "implementation_snapshot_sha256": None,
        "review_verdict": None,
        "blocked_from": None,
        "blocked_reason": None,
        "blocked_at": None,
        "created_at": now_iso(),
    }
    save_state(repo, args.task_id, state)
    print(target)


def command_status(args: argparse.Namespace) -> None:
    repo = repo_root(args.repo)
    print(json.dumps(load_state(repo, args.task_id), ensure_ascii=False, indent=2))


def command_approve(args: argparse.Namespace) -> None:
    if not args.user_approved:
        raise WorkflowError("approval requires --user-approved after explicit user confirmation")
    repo = repo_root(args.repo)
    state = load_state(repo, args.task_id)
    require_stage(state, "draft")
    plan = task_dir(repo, args.task_id) / "PLAN.md"
    require_complete_artifact(plan)
    text = plan.read_text(encoding="utf-8")
    match = re.search(
        r"(?ms)^## (?:待确认问题 / Open Questions|Open questions)\s*(.*?)(?=^## |\Z)",
        text,
    )
    if not match or match.group(1).strip() not in {"- 无。", "- None."}:
        raise WorkflowError('批准前，PLAN.md 的“待确认问题 / Open Questions”必须为“- 无。”')
    commit, dirty = git_snapshot(repo)
    if dirty:
        raise WorkflowError(
            "approval requires a clean non-task working tree; do not reset, clean, stash, "
            f"or discard changes automatically. current dirty={dirty}"
        )
    state.update(
        {
            "stage": "approved",
            "baseline_commit": commit,
            "baseline_dirty_files": [],
            "approved_plan_sha256": sha256(plan),
            "approved_at": now_iso(),
            "implementation_snapshot_sha256": None,
            "review_verdict": None,
            "blocked_from": None,
            "blocked_reason": None,
            "blocked_at": None,
        }
    )
    save_state(repo, args.task_id, state)
    print(f"approved plan v{state['plan_version']} sha256={state['approved_plan_sha256']}")


def command_revise_plan(args: argparse.Namespace) -> None:
    repo = repo_root(args.repo)
    state = load_state(repo, args.task_id)
    require_stage(state, "approved", "blocked", "changes_requested")
    target = task_dir(repo, args.task_id)
    plan = target / "PLAN.md"
    archive = target / "plans" / f"PLAN-v{state['plan_version']:03d}.md"
    archive.write_bytes(plan.read_bytes())
    state["plan_version"] += 1
    state["stage"] = "draft"
    state["approved_plan_sha256"] = None
    state["approved_at"] = None
    state["implementation_snapshot_sha256"] = None
    state["review_verdict"] = None
    state["blocked_from"] = None
    state["blocked_reason"] = None
    state["blocked_at"] = None
    save_state(repo, args.task_id, state)
    print(f"plan revision v{state['plan_version']} is now draft; previous plan archived at {archive}")


def command_begin_implementation(args: argparse.Namespace) -> None:
    repo = repo_root(args.repo)
    state = load_state(repo, args.task_id)
    if state["stage"] == "implementing":
        require_plan_hash(repo, args.task_id, state)
        require_baseline_commit(repo, state)
        report = task_dir(repo, args.task_id) / "implementations" / f"implementation-{state['cycle']:03d}.md"
        if not report.is_file():
            raise WorkflowError(f"implementing state has no current report: {report}")
        print(f"resuming implementation cycle {state['cycle']:03d}: {report}")
        return
    require_stage(state, "approved", "changes_requested")
    require_plan_hash(repo, args.task_id, state)
    if state["stage"] == "approved":
        require_baseline(repo, state)
    else:
        require_implementation_snapshot(repo, state)
    state["cycle"] += 1
    report = task_dir(repo, args.task_id) / "implementations" / f"implementation-{state['cycle']:03d}.md"
    if report.exists():
        raise WorkflowError(f"implementation report already exists: {report}")
    report.write_text(
        render(
            "IMPLEMENTATION.md",
            task_id=args.task_id,
            plan_version=state["plan_version"],
            cycle=f"{state['cycle']:03d}",
        ),
        encoding="utf-8",
    )
    state["stage"] = "implementing"
    state["implementation_snapshot_sha256"] = None
    state["review_verdict"] = None
    save_state(repo, args.task_id, state)
    print(report)


def command_complete_implementation(args: argparse.Namespace) -> None:
    repo = repo_root(args.repo)
    state = load_state(repo, args.task_id)
    require_stage(state, "implementing")
    require_plan_hash(repo, args.task_id, state)
    require_baseline_commit(repo, state)
    report = task_dir(repo, args.task_id) / "implementations" / f"implementation-{state['cycle']:03d}.md"
    require_complete_artifact(report)
    state["stage"] = "implemented"
    state["implementation_snapshot_sha256"] = working_tree_sha256(repo)
    save_state(repo, args.task_id, state)
    print(
        f"implementation cycle {state['cycle']:03d} completed "
        f"snapshot={state['implementation_snapshot_sha256']}"
    )


def command_begin_review(args: argparse.Namespace) -> None:
    repo = repo_root(args.repo)
    state = load_state(repo, args.task_id)
    if state["stage"] == "reviewing":
        require_plan_hash(repo, args.task_id, state)
        require_implementation_snapshot(repo, state)
        report = task_dir(repo, args.task_id) / "reviews" / f"review-{state['cycle']:03d}.md"
        if not report.is_file():
            raise WorkflowError(f"reviewing state has no current report: {report}")
        print(f"resuming review cycle {state['cycle']:03d}: {report}")
        return
    require_stage(state, "implemented")
    require_plan_hash(repo, args.task_id, state)
    require_implementation_snapshot(repo, state)
    report = task_dir(repo, args.task_id) / "reviews" / f"review-{state['cycle']:03d}.md"
    if report.exists():
        raise WorkflowError(f"review report already exists: {report}")
    report.write_text(
        render(
            "REVIEW.md",
            task_id=args.task_id,
            plan_version=state["plan_version"],
            cycle=f"{state['cycle']:03d}",
        ),
        encoding="utf-8",
    )
    state["stage"] = "reviewing"
    save_state(repo, args.task_id, state)
    print(report)


def command_complete_review(args: argparse.Namespace) -> None:
    repo = repo_root(args.repo)
    state = load_state(repo, args.task_id)
    require_stage(state, "reviewing")
    require_plan_hash(repo, args.task_id, state)
    require_implementation_snapshot(repo, state)
    report = task_dir(repo, args.task_id) / "reviews" / f"review-{state['cycle']:03d}.md"
    require_complete_artifact(report)
    expected = args.verdict.upper()
    text = report.read_text(encoding="utf-8")
    match = re.search(r"(?ms)^## (?:结论 / Verdict|Verdict)\s*(.*?)(?=^## |\Z)", text)
    if not match or match.group(1).strip() != expected:
        raise WorkflowError(f"review Verdict section must be exactly {expected}")
    state["stage"] = "passed" if args.verdict == "pass" else args.verdict
    state["review_verdict"] = args.verdict
    save_state(repo, args.task_id, state)
    print(f"review cycle {state['cycle']:03d} verdict={args.verdict}")


def command_block(args: argparse.Namespace) -> None:
    repo = repo_root(args.repo)
    state = load_state(repo, args.task_id)
    require_stage(state, "implementing", "reviewing")
    reason = args.reason.strip()
    if not reason:
        raise WorkflowError("blocked reason must not be empty")
    previous = state["stage"]
    state["stage"] = "blocked"
    state["blocked_from"] = previous
    state["blocked_reason"] = reason
    state["blocked_at"] = now_iso()
    state["review_verdict"] = None
    save_state(repo, args.task_id, state)
    print(f"task blocked from {previous}: {reason}")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    def common(name: str, handler) -> argparse.ArgumentParser:
        command = subparsers.add_parser(name)
        command.add_argument("--repo", default=".")
        command.add_argument("--task-id", required=True)
        command.set_defaults(handler=handler)
        return command

    init = common("init", command_init)
    init.add_argument("--title", required=True)
    common("status", command_status)
    approve = common("approve", command_approve)
    approve.add_argument("--user-approved", action="store_true")
    common("revise-plan", command_revise_plan)
    common("begin-implementation", command_begin_implementation)
    common("complete-implementation", command_complete_implementation)
    common("begin-review", command_begin_review)
    review = common("complete-review", command_complete_review)
    review.add_argument("--verdict", choices=("pass", "changes_requested", "blocked"), required=True)
    block = common("block", command_block)
    block.add_argument("--reason", required=True)
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    try:
        args.handler(args)
    except (WorkflowError, OSError, json.JSONDecodeError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
