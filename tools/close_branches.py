#!/usr/bin/env python3
"""Close every branch except vk264-phoenix (the single default branch).

Run after `gh api -X PATCH /repos/<owner>/<repo> -f default_branch=vk264-phoenix`
and after closing any open PRs. Uses `git ls-remote` for the authoritative
remote list (remote-tracking refs can be stale) and `git for-each-ref` for
clean local names.
"""
import subprocess, sys

KEEP = "vk264-phoenix"


def git(*args):
    return subprocess.run(["git", *args], capture_output=True, text=True)


def main():
    cur = git("branch", "--show-current").stdout.strip()
    if cur != KEEP:
        sys.exit(f"checkout {KEEP} first (currently on {cur})")

    heads = git("ls-remote", "--heads", "origin").stdout.splitlines()
    remotes = [line.split("refs/heads/", 1)[1] for line in heads
               if "refs/heads/" in line and line.split("refs/heads/", 1)[1] != KEEP]
    print(f"remote branches to delete: {len(remotes)} -> {remotes}")
    failed = []
    for i in range(0, len(remotes), 40):
        batch = remotes[i:i + 40]
        r = git("push", "origin", "--delete", *batch)
        print(f"  batch {i//40+1}: rc={r.returncode}")
        if r.returncode:
            failed.extend(batch)
            print((r.stdout + r.stderr)[-1000:])

    git("fetch", "origin", "--prune")
    git("remote", "set-head", "origin", KEEP)
    locals_ = [x for x in git("for-each-ref", "--format=%(refname:short)",
                              "refs/heads").stdout.splitlines() if x and x != KEEP]
    print(f"local branches to delete: {len(locals_)}")
    for name in locals_:
        r = git("branch", "-D", name)
        if r.returncode:
            print("  local FAILED", name, (r.stdout + r.stderr)[-300:])

    print("remaining local:", git("branch").stdout.strip().replace("\n", " "))
    print("remaining remote:", git("branch", "-r").stdout.strip().replace("\n", " "))
    print("failed remote deletions:", len(failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
