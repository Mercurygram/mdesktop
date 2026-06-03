#!/usr/bin/env python3

import re
import os
import datetime
from xml.etree import ElementTree as ET
import argparse

def read_app_version_str():
    # build/version sits next to this script. Mercurygram release tags encode a
    # 4-part display version (X.Y.Z.N); set_release_version.py patches
    # AppVersionStr there. The changelog only carries the upstream 3-part header,
    # so without this the AppStream/Flatpak version drops the fork counter.
    version_path = os.path.join(os.path.dirname(os.path.realpath(__file__)),
                                'version')
    try:
        with open(version_path, 'r', encoding='utf-8') as f:
            for line in f:
                m = re.match(r'AppVersionStr\s+(\S+)', line)
                if m is not None:
                    return m.group(1)
    except OSError:
        pass
    return None

def parse_changelog(changelog_path):
    version_re = re.compile(r'([\d.-]+)\s+(\w+)?\s*\((\d{2}.\d{2}\.\d{2})\)')
    entry_re = re.compile(r'-\s(.*)')

    with open(changelog_path, "r", encoding="utf-8") as f:
        changelog_lines = f.read().splitlines()

    releases = []
    for l in changelog_lines:
        version_match = version_re.match(l)
        entry_match = entry_re.match(l)
        if version_match is not None:
            version, prerelease, date = version_match.groups()
            release = (version,
                       prerelease,
                       datetime.datetime.strptime(date, '%d.%m.%y').date(),
                       [])
            releases.append(release)
        elif entry_match is not None:
            release[3].append(entry_match.group(1))

    return releases

def get_release_xml(version, prerelease, date, changes):
    release = ET.Element("release")
    if prerelease is None:
        ver_str = version
    else:
        ver_str = f"{version}~{prerelease}"
    release.set("version", ver_str)
    release.set("date", date.isoformat())
    description = ET.SubElement(release, "description")
    changelist = ET.SubElement(description, "ul")
    for c in changes:
        change = ET.SubElement(changelist, "li")
        change.text = c
    return release

def get_changelog_xml(changelog, max_items=None):
    releases = ET.Element("releases")
    if max_items is not None:
        changelog = changelog[:max_items]
    for rel in changelog:
        release = get_release_xml(*rel)
        releases.append(release)
    return releases

def apply_full_version(changelog):
    # Promote the newest entry's version to the full 4-part AppVersionStr when it
    # extends the 3-part changelog header (e.g. header 6.9.3 + AppVersionStr
    # 6.9.3.2 -> 6.9.3.2). Dev builds keep a 3-part AppVersionStr, so the prefix
    # test fails and nothing changes.
    if not changelog:
        return changelog
    full = read_app_version_str()
    if full is None:
        return changelog
    newest = changelog[0]
    if full.startswith(newest[0] + '.'):
        changelog[0] = (full,) + tuple(newest[1:])
    return changelog

def update_metadata(metadata_path, changelog, max_items=None):
    changelog = apply_full_version(changelog)
    metadata = ET.parse(metadata_path)
    root = metadata.getroot()
    releases = root.find("releases")
    if releases is not None:
        root.remove(releases)
    root.append(
        get_changelog_xml(changelog, max_items)
    )
    metadata.write(metadata_path, encoding="utf-8", xml_declaration=True)

def main():
    ap = argparse.ArgumentParser("Parse Telegram changelog")
    ap.add_argument("-c", "--changelog-path", default="changelog.txt")
    ap.add_argument("-m", "--metadata-path", default="lib/xdg/it.belloworld.mercurygram.metainfo.xml")
    ap.add_argument("-n", "--num-releases", type=int, default=None)
    args = ap.parse_args()
    update_metadata(args.metadata_path,
                   parse_changelog(args.changelog_path),
                   max_items=args.num_releases)

if __name__ == "__main__":
    main()
