#!/usr/bin/env python3
"""Archive the pre-Phoenix branches as tags before deleting the branches.

All old work was merged through PRs; GitHub keeps refs/pull/<n>/head even after
a branch is deleted. This script fetches those refs, creates one lightweight
tag per old branch (`archive/pr<N>-<branch>`), plus `archive/pre-phoenix-main`,
pushes the tags, and drops the temporary PR-tracking refs. No branches are
created: vk264-phoenix remains the only branch.
"""
import json, re, subprocess, sys


def run(*args, capture=True):
    return subprocess.run(args, capture_output=capture, text=True)


def main():
    r = run('git', 'fetch', 'origin', '+refs/pull/*/head:refs/remotes/pr/*')
    if r.returncode:
        print(r.stderr[-1500:]); sys.exit(1)

    r = run('gh', 'pr', 'list', '--state', 'all', '--limit', '500',
            '--json', 'number,headRefName')
    if r.returncode:
        print(r.stderr[-1500:]); sys.exit(1)
    prs = json.loads(r.stdout)

    created = []
    for pr in prs:
        n = pr['number']
        slug = re.sub(r'[^A-Za-z0-9._-]+', '-', pr['headRefName']).strip('-')[:50]
        tag = f'archive/pr{n}-{slug}' if slug else f'archive/pr{n}'
        r = run('git', 'tag', tag, f'refs/remotes/pr/{n}')
        if r.returncode:
            print('tag FAILED', tag, r.stderr[-300:])
        else:
            created.append(tag)

    r = run('git', 'tag', 'archive/pre-phoenix-main', '212def0')
    if not r.returncode:
        created.append('archive/pre-phoenix-main')
    print(f'created {len(created)} archive tags')

    r = run('git', 'push', 'origin', '--tags')
    if r.returncode:
        print(r.stdout[-1200:], r.stderr[-1200:]); sys.exit(1)

    for ref in run('git', 'for-each-ref', '--format=%(refname)',
                   'refs/remotes/pr').stdout.splitlines():
        run('git', 'update-ref', '-d', ref)
    print('local tags:', len(run('git', 'tag').stdout.splitlines()))
    print('branches:', run('git', 'branch', '-a').stdout.strip().replace('\n', ' '))
    return 0


if __name__ == '__main__':
    sys.exit(main())
