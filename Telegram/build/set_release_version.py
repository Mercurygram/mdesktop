'''
This file is part of Mercurygram Desktop,
an unofficial privacy/security focused fork of Telegram Desktop.

Sets the build version from a Mercurygram release tag.

Mercurygram release tags carry an upstream base plus a fork counter:
  vX.Y.Z.N        -> stable release
  vX.Y.Z.N-beta   -> beta release (GitHub prerelease)

tdesktop only exposes a real 4th component through its closed-alpha path, so we
keep the *display* string 4-part (X.Y.Z.N) while encoding a monotonic 32-bit
AppVersion the updater can compare:

  AppVersion = (major * 100 + minor) * 10000 + patch * 100 + counter

Caps: minor < 100, patch < 100, counter < 100 (plenty for realistic versions).
Stable uses the stable update key; beta sets BetaChannel/AppBetaVersion (beta
update key). The closed-alpha machinery stays disabled (AlphaVersion = 0).

Unlike set_version.py this does NOT gate on a changelog entry and does NOT treat
the 4th component as an alpha counter -- both would break the release flow.
'''
import sys, os, re

scriptPath = os.path.dirname(os.path.realpath(__file__))

inputTag = ''
githubOutput = False
for arg in sys.argv[1:]:
    a = arg.strip()
    if not a:
        continue
    if a == '--github-output':
        githubOutput = True
    elif not inputTag:
        inputTag = a

match = re.match(r'^v?(\d+)\.(\d+)\.(\d+)\.(\d+)(-beta)?$', inputTag)
if not match:
    print('Bad release tag: "' + inputTag
        + '" (expected vX.Y.Z.N or vX.Y.Z.N-beta)', file=sys.stderr)
    sys.exit(1)

versionMajor = int(match.group(1))
versionMinor = int(match.group(2))
versionPatch = int(match.group(3))
versionCounter = int(match.group(4))
versionBeta = bool(match.group(5))

for name, value in (
        ('major', versionMajor),
        ('minor', versionMinor),
        ('patch', versionPatch),
        ('counter', versionCounter)):
    if value >= 100:
        print('Version part too large (max 99): ' + name + '=' + str(value),
            file=sys.stderr)
        sys.exit(1)

versionFull = (versionMajor * 100 + versionMinor) * 10000 \
    + versionPatch * 100 + versionCounter

parts = [
    str(versionMajor),
    str(versionMinor),
    str(versionPatch),
    str(versionCounter),
]
versionStr = '.'.join(parts)
versionStrMajor = parts[0] + '.' + parts[1]
versionOriginal = versionStr + ('.beta' if versionBeta else '')

if githubOutput:
    # Emit the values the release workflow's `meta` job exports, then stop --
    # the formula and validation above stay the single source of truth.
    print('version=' + versionStr)
    print('appversion=' + str(versionFull))
    print('channel=' + ('beta' if versionBeta else 'stable'))
    print('prerelease=' + ('true' if versionBeta else 'false'))
    sys.exit(0)

print('Setting version: ' + versionStr
    + (' beta' if versionBeta else ' stable')
    + ' (AppVersion ' + str(versionFull) + ')')


def replaceInFile(path, replacements):
    content = ''
    foundReplacements = {}
    updated = False
    with open(path, 'r') as f:
        for line in f:
            for replacement in replacements:
                if re.search(replacement[0], line):
                    changed = re.sub(replacement[0], replacement[1], line)
                    if changed != line:
                        line = changed
                        updated = True
                    foundReplacements[replacement[0]] = True
            content = content + line
    for replacement in replacements:
        if not replacement[0] in foundReplacements:
            print('Could not find "' + replacement[0] + '" in "' + path + '".')
            sys.exit(1)
    if updated:
        with open(path, 'w') as f:
            f.write(content)


print('Patching build/version...')
replaceInFile(scriptPath + '/version', [
    [r'(AppVersion\s+)\d+', r'\g<1>' + str(versionFull)],
    [r'(AppVersionStrMajor\s+)\d[\d\.]*', r'\g<1>' + versionStrMajor],
    [r'(AppVersionStrSmall\s+)\d[\d\.]*', r'\g<1>' + versionStr],
    [r'(AppVersionStr\s+)\d[\d\.]*', r'\g<1>' + versionStr],
    [r'(BetaChannel\s+)\d', r'\g<1>' + ('1' if versionBeta else '0')],
    [r'(AlphaVersion\s+)\d+', r'\g<1>0'],
    [r'(AppVersionOriginal\s+)\S+', r'\g<1>' + versionOriginal],
])

print('Patching core/version.h...')
replaceInFile(scriptPath + '/../SourceFiles/core/version.h', [
    [r'(AppVersion\s+=\s+)\d+', r'\g<1>' + str(versionFull)],
    [r'(AppVersionStr\s+=\s+)[^;]+', r'\g<1>"' + versionStr + '"'],
    [r'(AppBetaVersion\s+=\s+)[a-z]+',
        r'\g<1>' + ('true' if versionBeta else 'false')],
])

withcomma = ','.join(parts)
withdot = '.'.join(parts)
rcReplaces = [
    [r'(FILEVERSION\s+)\d+,\d+,\d+,\d+', r'\g<1>' + withcomma],
    [r'(PRODUCTVERSION\s+)\d+,\d+,\d+,\d+', r'\g<1>' + withcomma],
    [r'("FileVersion",\s+)"\d+\.\d+\.\d+\.\d+"', r'\g<1>"' + withdot + '"'],
    [r'("ProductVersion",\s+)"\d+\.\d+\.\d+\.\d+"', r'\g<1>"' + withdot + '"'],
]

print('Patching Telegram.rc...')
replaceInFile(scriptPath + '/../Resources/winrc/Telegram.rc', rcReplaces)

print('Patching Updater.rc...')
replaceInFile(scriptPath + '/../Resources/winrc/Updater.rc', rcReplaces)

print('Patching appxmanifest.xml...')
replaceInFile(scriptPath + '/../Resources/uwp/AppX/AppxManifest.xml', [
    [r'( Version=)"\d+\.\d+\.\d+\.\d+"', r'\g<1>"' + withdot + '"'],
])

print('Done.')
