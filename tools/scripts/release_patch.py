#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

# Copyright (C) 2026, ArtInChip Technology Co., Ltd
#
# Convert commits from repo-managed sub-projects into patches suitable for
# applying to a monolithic git repository via "git am".
#
# Usage:
#   # List all available sub-projects
#   python3 tools/scripts/release_patch.py --list
#
#   # Generate patches for all projects since a tag
#   python3 tools/scripts/release_patch.py --tag LUBAN_LITE_V1.3.1b
#
#   # Generate patches since a specific commit SHA (the commit itself is
#   # included in the output)
#   python3 tools/scripts/release_patch.py --tag 39356c3c
#
#   # Generate patches only for specific sub-projects
#   python3 tools/scripts/release_patch.py --tag LUBAN_LITE_V1.3.1b \
#       --projects "bsp/artinchip,bsp/artinchip/hal,target"
#
#   # Specify output directory
#   python3 tools/scripts/release_patch.py --tag LUBAN_LITE_V1.3.1b \
#       --output /tmp/release_patches
#
#   # Emit at most N patches (the earliest ones from the start ref, per project)
#   python3 tools/scripts/release_patch.py -t a49dc215 -p tools -n 10
#
# Note:
#   - Must be run from repo workspace root (contains .repo/manifest.xml)
#   - All sub-projects are expected to use the same tag/ref; sub-projects
#     without the given ref are skipped automatically

import os
import sys
import argparse
import subprocess
import tempfile
import shutil
import glob
import re
from email.utils import parsedate_to_datetime
from datetime import datetime
import xml.etree.ElementTree as ET

VERBOSE = False


def pr_info(msg):
    print(f">>> {msg}")


def pr_warn(msg):
    print(f"!!! {msg}")


def pr_err(msg):
    print(f"*** {msg}")


def _parse_manifest_file(file_path):
    """Parse a single manifest XML file and return list of project dicts."""
    tree = ET.parse(file_path)
    root = tree.getroot()
    projects = []

    for proj_elem in root.findall('project'):
        name = proj_elem.get('name')
        path = proj_elem.get('path', '')

        linkfiles = []
        for lf in proj_elem.findall('linkfile'):
            src = lf.get('src', '')
            dest = lf.get('dest', '')
            if src and dest:
                linkfiles.append((src, dest))

        projects.append({
            'name': name,
            'path': path.rstrip('/'),
            'linkfiles': linkfiles,
        })

    return projects


def parse_manifest(manifest_path):
    """Parse repo manifest XML, following <include> directives.

    Repo uses <include name="file.xml"/> to reference additional manifest
    files located in .repo/manifests/. This function resolves those includes.

    Args:
        manifest_path: Path to .repo/manifest.xml

    Returns:
        list of dicts, each with:
            'name': project name
            'path': project path in workspace (e.g. 'bsp/artinchip/')
            'linkfiles': list of (src, dest) tuples for path remapping
    """
    # manifests/ directory for included manifests
    # manifest_path is .repo/manifest.xml, its parent is .repo/
    repo_dir = os.path.dirname(os.path.realpath(manifest_path))
    manifests_dir = os.path.join(repo_dir, 'manifests')

    tree = ET.parse(manifest_path)
    root = tree.getroot()

    all_projects = []

    # Parse the main manifest first
    all_projects.extend(_parse_manifest_file(manifest_path))

    # Follow <include> elements — repo resolves them relative to .repo/manifests/
    for inc in root.findall('include'):
        inc_name = inc.get('name', '')
        if inc_name:
            inc_path = os.path.join(manifests_dir, inc_name)
            if os.path.exists(inc_path):
                all_projects.extend(_parse_manifest_file(inc_path))
            else:
                pr_warn(f'Included manifest not found: {inc_path}')

    return all_projects


def get_subject_prefix(project_path):
    """Generate subject prefix from project path.

    Follows existing release_patch.py convention:
    take last component of path, split by '-', take first part.

    Examples:
        'bsp/artinchip' -> 'artinchip'
        'bsp/artinchip/hal' -> 'hal'
        'kernel/rt-thread' -> 'rt'
        'target' -> 'target'
    """
    last = project_path.rstrip('/').split('/')[-1]
    return last.split('-')[0]


def transform_patch_path(file_path, project_path, linkfiles):
    """Transform a file path from repo sub-project to monorepo path.

    Args:
        file_path: Original file path in the patch (e.g. 'drv/pm/aic_drv_pm.c')
        project_path: Project's path in workspace (e.g. 'bsp/artinchip')
        linkfiles: List of (src, dest) tuples

    Returns:
        Transformed file path for monorepo (e.g. 'bsp/artinchip/drv/pm/aic_drv_pm.c')
    """
    # Check linkfile mappings first (prefix match)
    for src, dest in linkfiles:
        # For exact file match
        if file_path == src:
            return dest
        # For directory prefix match (src is a directory name)
        if file_path.startswith(src + '/'):
            return dest.rstrip('/') + '/' + file_path[len(src) + 1:]

    # Default: prepend project path
    if project_path:
        return project_path + '/' + file_path
    return file_path


def scan_patch_date(input_path):
    """Scan only Date header from a patch file.

    Reads only the header portion (up to the first '---' separator)
    to avoid loading the entire patch body into memory.

    Returns datetime.min if parsing fails.
    """
    with open(input_path, 'r', encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith('Date: '):
                date_str = line[len('Date: '):].strip()
                try:
                    return parsedate_to_datetime(date_str)
                except Exception:
                    return datetime.min
            # Stop reading at the '---' separator (end of header)
            if line.rstrip() == '---':
                return datetime.min
    return datetime.min


def scan_patch_commit_sha(input_path):
    """Extract the source commit SHA from a patch file's 'From' line.

    git format-patch emits 'From <sha> <date>' as the very first line. Only
    the header is read to avoid loading the full patch body.

    Returns None if not found.
    """
    with open(input_path, 'r', encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith('From '):
                parts = line[len('From '):].split()
                if parts and re.fullmatch(r'[0-9a-fA-F]{7,40}', parts[0]):
                    return parts[0]
                return None
            # Stop at the header/body separator
            if line.rstrip() == '---':
                return None
    return None


def get_committer_date(project_path, commit_sha):
    """Return a commit's committer datetime via git, or None on failure.

    The committer date (not the author date carried in the patch 'Date:'
    header) is monotonically increasing along the topological history after
    gerrit rebases, so it yields a correct, am-able patch order.
    """
    if not commit_sha:
        return None
    result = subprocess.run(
        ['git', 'show', '-s', '--format=%ct', commit_sha],
        cwd=project_path,
        capture_output=True, text=True
    )
    if result.returncode != 0:
        return None
    try:
        return datetime.fromtimestamp(int(result.stdout.strip()))
    except (ValueError, OSError):
        return None


def transform_patch_file(input_path, output_path, project_path, linkfiles, prefix):
    """Stream-transform a patch file: path remapping + subject prefix.

    Reads input_path line-by-line and writes to output_path. This avoids
    loading large patches (e.g. 1+ GB) fully into memory.

    Args:
        input_path: Original patch file from git format-patch
        output_path: Transformed patch file destination
        project_path: Project path prefix (e.g. 'bsp/artinchip')
        linkfiles: List of (src, dest) tuples for path remapping
        prefix: Subject prefix (e.g. 'artinchip')
    """
    state = {'in_diffstat': False, 'in_diff_section': False}
    with open(input_path, 'r', encoding='utf-8', errors='replace') as fin:
        with open(output_path, 'w', encoding='utf-8') as fout:
            for line in fin:
                line = line.rstrip('\n')
                new_line = _transform_line(line, project_path, linkfiles, prefix, state)
                fout.write(new_line + '\n')


def _transform_line(line, project_path, linkfiles, prefix, state=None):
    """Transform a single patch line.

    Uses a mutable state dict to track patch sections:
      - in_diffstat: between '---' separator and first 'diff --git'
      - in_diff_section: from first 'diff --git' onwards
    This prevents diffstat path transformation from matching context lines
    inside diff hunks.
    """
    if state is None:
        state = {'in_diffstat': False, 'in_diff_section': False}

    # --- separator (between commit message and diffstat)
    if line.rstrip() == '---' and not state['in_diff_section']:
        state['in_diffstat'] = True
        return line

    # diff --git marks the start of the actual diff section
    if line.startswith('diff --git a/'):
        state['in_diffstat'] = False
        state['in_diff_section'] = True
        match = re.match(r'^diff --git a/(.*) b/(.*)$', line)
        if match:
            new_a = transform_patch_path(match.group(1), project_path, linkfiles)
            new_b = transform_patch_path(match.group(2), project_path, linkfiles)
            return f'diff --git a/{new_a} b/{new_b}'

    # Subject line transformation
    if line.startswith('Subject: '):
        subject = line[len('Subject: '):]
        clean = re.sub(r'^\[PATCH(\s+\d+/\d+)?\]\s*', '', subject)
        return f'Subject: [PATCH] {prefix}: {clean}'

    # --- a/<path>
    if line.startswith('--- a/'):
        old_path = line[6:]
        new_path = transform_patch_path(old_path, project_path, linkfiles)
        return '--- a/' + new_path

    # +++ b/<path>
    if line.startswith('+++ b/'):
        old_path = line[6:]
        new_path = transform_patch_path(old_path, project_path, linkfiles)
        return '+++ b/' + new_path

    # diffstat line: ' path/to/file | 10 +++----'
    # Only apply inside diffstat section to avoid false matches in diff hunks.
    # Also handles rename entries: ' {old => new}/file | 10 ++'
    if state['in_diffstat']:
        diffstat_match = re.match(r'^(\s+)(.+)(\s+\|\s+.*)$', line)
        if diffstat_match:
            indent = diffstat_match.group(1)
            path_part = diffstat_match.group(2)
            stat_part = diffstat_match.group(3)
            # Handle rename: {old => new}/rest or path/{old => new}/rest
            rename_match = re.match(
                r'^(.*)\{(.+?) => (.+?)\}(/.*)?$', path_part)
            if rename_match:
                pre = rename_match.group(1)
                new_name = rename_match.group(3)
                post = rename_match.group(4) or ''
                file_path = pre + new_name + post
                new_path = transform_patch_path(
                    file_path, project_path, linkfiles)
                return f'{indent}{{{new_path} => {new_path}}}{stat_part}'
            else:
                new_path = transform_patch_path(
                    path_part, project_path, linkfiles)
                return f'{indent}{new_path}{stat_part}'

    return line


def generate_patches(project, base_ref, tmpdir, max_count=None):
    """Generate format-patch files for a project since the given ref.

    Args:
        project: Project dict from parse_manifest()
        base_ref: Git ref to start from (tag, commit SHA, or branch). A raw
            commit SHA is treated as inclusive (the commit itself is emitted);
            a tag or branch ref is treated as exclusive (only later commits).
        tmpdir: Directory to store generated patches
        max_count: If set, emit at most this many patches, keeping the EARLIEST
            ones starting from base_ref (None = no limit)

    Returns:
        List of patch file paths
    """
    project_path = project['path']
    if not os.path.exists(project_path):
        pr_warn(f"Project path not found: {project_path}, skipping")
        return []

    if not os.path.exists(os.path.join(project_path, '.git')):
        pr_warn(f"Not a git repository: {project_path}, skipping")
        return []

    # Resolve the ref to a concrete commit. Peeling with '^{commit}' handles
    # annotated tags (which otherwise resolve to a tag object) and short SHAs.
    result = subprocess.run(
        ['git', 'rev-parse', '--verify', f'{base_ref}^{{commit}}'],
        cwd=project_path,
        capture_output=True, text=True
    )
    if result.returncode != 0:
        pr_info(f"Ref '{base_ref}' not found in {project['name']}, skipping")
        return []
    base_commit = result.stdout.strip()

    # 'REF..HEAD' only means 'commits after REF' when REF is actually an
    # ancestor of HEAD. A raw commit SHA may come from another branch or from
    # a rebased/amended history (common in repo/gerrit workflows); in that case
    # the range silently yields the wrong starting point. Reject such refs
    # explicitly instead of generating misplaced patches.
    result = subprocess.run(
        ['git', 'merge-base', '--is-ancestor', base_commit, 'HEAD'],
        cwd=project_path,
        capture_output=True, text=True
    )
    if result.returncode != 0:
        pr_warn(f"Ref '{base_ref}' ({base_commit[:8]}) is not an ancestor of "
                f"HEAD in {project['name']}, skipping (wrong start point)")
        return []

    # Decide whether the boundary commit itself belongs to the patch set.
    #   - A raw commit SHA   -> INCLUSIVE: patches start *at* that commit.
    #   - A tag / branch ref -> EXCLUSIVE: the ref marks the last released
    #     point, so only commits strictly after it are emitted.
    is_sha = re.fullmatch(r'[0-9a-fA-F]{4,40}', base_ref) is not None
    base_is_root = False
    if is_sha:
        # Inclusive is expressed as 'base^..HEAD'. When base is the repository
        # root commit (no parent) fall back to '--root' so it is still emitted.
        parent = subprocess.run(
            ['git', 'rev-parse', '--verify', '-q', f'{base_commit}^'],
            cwd=project_path,
            capture_output=True, text=True
        )
        if parent.returncode == 0 and parent.stdout.strip():
            list_spec = [f'{base_commit}^..HEAD']
        else:
            base_is_root = True
            list_spec = ['HEAD']
    else:
        list_spec = [f'{base_commit}..HEAD']

    # List the commits in the range oldest-first (topological) so that -n keeps
    # the EARLIEST max_count commits starting at base, not the most recent ones
    # (which is what 'git format-patch -n' would do).
    result = subprocess.run(
        ['git', 'rev-list', '--reverse', '--topo-order'] + list_spec,
        cwd=project_path,
        capture_output=True, text=True
    )
    commits = result.stdout.split()
    if not commits:
        pr_info(f"No commits since '{base_ref}' in {project['name']}")
        return []

    # Converge the range end on the Nth commit when a limit is set.
    truncated = max_count is not None and len(commits) > max_count
    if truncated:
        end_commit = commits[max_count - 1]
    else:
        end_commit = commits[-1]

    if is_sha and base_is_root:
        patch_range = ['--root', end_commit]
    elif is_sha:
        patch_range = [f'{base_commit}^..{end_commit}']
    else:
        patch_range = [f'{base_commit}..{end_commit}']

    # Generate patches (--topo-order keeps git's own numbering strictly
    # parent-before-child, which is what 'git am' requires)
    result = subprocess.run(
        ['git', 'format-patch', '--topo-order'] + patch_range +
        ['-o', tmpdir, '--no-renames'],
        cwd=project_path,
        capture_output=True, text=True
    )

    if result.returncode != 0:
        pr_err(f"git format-patch failed in {project_path}: {result.stderr}")
        return []

    patches = glob.glob(os.path.join(tmpdir, '*.patch'))
    if patches:
        extra = f' (limited to first {max_count})' if truncated else ''
        pr_info(f"Generated {len(patches)} patch(es) from {project['name']}{extra}")
    return sorted(patches)


def process_project(project, base_ref, stage_dir, max_count=None):
    """Generate and transform patches for one project.

    Patches are stream-transformed to avoid loading large patches into memory,
    then written to stage_dir with date-based names for chronological sorting.

    Returns:
        Number of patches generated for this project.
    """
    tmpdir = tempfile.mkdtemp(prefix='release_proj_')
    try:
        patch_files = generate_patches(project, base_ref, tmpdir, max_count)
        prefix = get_subject_prefix(project['path'])

        for pf in patch_files:
            orig_name = os.path.basename(pf)

            # Keep git's own sequence number (topological order within this
            # project) as a tie-breaker so same-second commits still apply in
            # parent-before-child order.
            gitnum = '9999'
            num_match = re.match(r'^(\d{4})-(.+)$', orig_name)
            if num_match:
                gitnum = num_match.group(1)
                orig_name = num_match.group(2)

            # Sort key: committer date (monotonic along history after gerrit
            # rebases); fall back to the patch's author 'Date:' header only if
            # the committer lookup fails.
            sha = scan_patch_commit_sha(pf)
            date = get_committer_date(project['path'], sha) or scan_patch_date(pf)
            ts = date.strftime('%Y%m%d%H%M%S')

            # Stream-transform patch to stage directory
            stage_name = f'{ts}_{gitnum}_{prefix}_{orig_name}'
            stage_path = os.path.join(stage_dir, stage_name)

            transform_patch_file(
                pf, stage_path, project['path'],
                project['linkfiles'], prefix
            )

        return len(patch_files)
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(
        description='Convert repo sub-project commits to git(monorepo) format-patch patches',
        epilog='See docs/release_patch_guide.md for detailed usage guide.',
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('-t', '--tag', type=str, default=None,
                        metavar='REF',
                        help='Git ref to start from: tag, commit SHA, or branch. '
                             'A commit SHA is inclusive (that commit is emitted '
                             'too); a tag/branch is exclusive (only later commits '
                             'in REF..HEAD). '
                             'Example: -t LUBAN_LITE_V1.3.1b or -t 39356c3c')
    parser.add_argument('-p', '--projects', type=str, default=None,
                        metavar='PATH[,PATH...]',
                        help='Only process specified sub-projects (comma-separated). '
                             'Default: all projects. '
                             'Example: -p "bsp/artinchip,target"')
    parser.add_argument('-o', '--output', type=str, default=None,
                        metavar='DIR',
                        help='Output directory for patches. Default: patch_output_<date>/')
    parser.add_argument('-n', '--count', type=int, default=None, metavar='N',
                        help='Emit at most N patches per project, keeping the '
                             'earliest ones starting from -t REF. '
                             'Default: no limit. Example: -n 10')
    parser.add_argument('-l', '--list', action='store_true',
                        help='List all sub-project paths and linkfile mappings, then exit')
    parser.add_argument('-v', '--verbose', action='store_true',
                        help='Enable verbose output')
    args = parser.parse_args()

    global VERBOSE
    VERBOSE = args.verbose

    # Verify we are in a repo workspace
    manifest_path = '.repo/manifest.xml'
    if not os.path.exists(manifest_path):
        pr_err('Current directory is not the root of a repo workspace')
        pr_err(f'Expected {manifest_path} not found')
        sys.exit(1)

    repo_root = os.path.abspath('.')
    pr_info(f'Repo workspace root: {repo_root}')

    # Parse manifest
    projects = parse_manifest(manifest_path)
    pr_info(f'Found {len(projects)} projects in manifest')

    # --list: show available projects and exit
    if args.list:
        print()
        print('Available projects:')
        for p in projects:
            lf_info = ', '.join(f'{s}->{d}' for s, d in p['linkfiles']) or 'none'
            print(f'  {p["path"]:<35s} (linkfiles: {lf_info})')
        sys.exit(0)

    # Require --tag when not using --list
    if not args.tag:
        pr_err('--tag is required (accepts tag, commit SHA, or branch ref)')
        sys.exit(1)

    if args.count is not None and args.count <= 0:
        pr_err('--count must be a positive integer')
        sys.exit(1)

    # Filter by --projects if specified
    if args.projects:
        def norm_path(s):
            # Tolerate stray leading/trailing/duplicate slashes, e.g. 'a/b/'
            return '/'.join(seg for seg in s.replace('\\', '/').split('/') if seg)

        # Map normalized manifest path -> project for tolerant matching.
        by_norm = {norm_path(p['path']): p for p in projects}

        selected = []
        for s in args.projects.split(','):
            key = norm_path(s.strip())
            if not key:
                continue
            if key in selected:
                continue
            if key not in by_norm:
                pr_warn(f'Project "{s.strip()}" not found in manifest')
                continue
            selected.append(key)

        filtered = [by_norm[k] for k in selected]
        if not filtered:
            pr_err('No matching projects found. Use --list to see available projects.')
            sys.exit(1)
        projects = filtered
        pr_info(f'Filtered to {len(projects)} project(s)')

    # Create output directory
    if args.output:
        output_dir = args.output
    else:
        date_str = datetime.now().strftime('%Y-%m-%d')
        output_dir = f'patch_output_{date_str}'

    os.makedirs(output_dir, exist_ok=True)
    pr_info(f'Output directory: {output_dir}')

    # Staging directory for transformed patches
    stage_dir = tempfile.mkdtemp(prefix='release_stage_')

    try:
        total = 0
        # Process each project, transforming and staging patches immediately
        for project in projects:
            count = process_project(project, args.tag, stage_dir, args.count)
            total += count

        if total == 0:
            pr_err(f'No patches generated. Does ref "{args.tag}" exist in any project?')
            sys.exit(1)

        pr_info(f'Total patches collected: {total}')
        print()

        # Sort staged patches by filename (date prefix ensures chronological order)
        staged = sorted(glob.glob(os.path.join(stage_dir, '*.patch')))

        # Renumber and copy to output
        for i, sp in enumerate(staged):
            seq = f'{i + 1:04d}'
            basename = os.path.basename(sp)
            # Strip the sort prefix (format: YYYYMMDDHHmmSS_<gitnum>_)
            name_match = re.match(r'^\d{14}_\d{4}_(.+)$', basename)
            if name_match:
                basename = name_match.group(1)

            out_name = f'{seq}-{basename}'
            out_path = os.path.join(output_dir, out_name)
            shutil.copy2(sp, out_path)
            pr_info(f'  [{seq}] {out_name}')

        print()
        pr_info(f'Done! {total} patches written to: {output_dir}')
        pr_info('Apply them in the monolithic repo with:')
        pr_info(f'  cd <monorepo> && git am {os.path.abspath(output_dir)}/*.patch')

    finally:
        shutil.rmtree(stage_dir, ignore_errors=True)


if __name__ == '__main__':
    main()
