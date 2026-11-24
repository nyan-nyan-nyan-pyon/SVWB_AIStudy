"""Publish run artifacts atomically and tolerate transient Windows locks."""

from pathlib import Path
import time


def retry_windows_lock(operation):
    for attempt in range(11):
        try:
            return operation()
        except OSError as error:
            if attempt == 10 or getattr(error, "winerror", None) not in (5, 32, 33):
                raise
            time.sleep((attempt + 1) / 5)


def publish_checkpoint(stage: Path, target: Path):
    def promote():
        if target.exists():
            raise FileExistsError(f"Checkpoint already exists: {target}")
        stage.rename(target)

    retry_windows_lock(promote)


def replace_file(source: Path, target: Path):
    retry_windows_lock(lambda: source.replace(target))
