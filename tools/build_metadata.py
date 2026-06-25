import datetime
import os
import subprocess

Import("env")


def run_git(args):
    try:
        return subprocess.check_output(
            ["git", *args],
            cwd=env.subst("$PROJECT_DIR"),
            stderr=subprocess.DEVNULL,
            text=True,
        ).strip()
    except Exception:
        return ""


def build_commit():
    return (
        os.environ.get("CI_COMMIT_SHORT_SHA", "").strip()
        or os.environ.get("GIT_COMMIT", "").strip()[:12]
        or run_git(["rev-parse", "--short=12", "HEAD"])
        or "unknown"
    )


def build_date():
    ci_timestamp = os.environ.get("CI_COMMIT_TIMESTAMP", "").strip()
    if ci_timestamp:
        return ci_timestamp

    source_date_epoch = os.environ.get("SOURCE_DATE_EPOCH", "").strip()
    if source_date_epoch:
        try:
            timestamp = int(source_date_epoch)
            return datetime.datetime.fromtimestamp(timestamp, datetime.timezone.utc).isoformat().replace("+00:00", "Z")
        except ValueError:
            pass

    git_timestamp = run_git(["show", "-s", "--format=%cI", "HEAD"])
    if git_timestamp:
        return git_timestamp

    return "unknown"


env.Append(
    CPPDEFINES=[
        ("BUILD_GIT_COMMIT", env.StringifyMacro(build_commit())),
        ("BUILD_DATE", env.StringifyMacro(build_date())),
    ]
)
