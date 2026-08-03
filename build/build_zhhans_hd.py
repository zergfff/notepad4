import os
import shutil
import subprocess
import sys
import tempfile

# Build the Simplified Chinese (zh-Hans) Hi-DPI release for AVX2 and pack it
# into Notepad4_HD_zh-Hans_AVX2_v<version>.zip
#
# Run from anywhere:  python build/build_zhhans_hd.py
#
# This script temporarily replaces src/config.h and the localized resource
# files, then restores them afterwards so the working tree stays clean.

buildFolder = os.path.dirname(os.path.abspath(__file__))
projectDir = os.path.join(buildFolder, 'VisualStudio')
localeDir = os.path.join(buildFolder, '..', 'locale')

notepad4_config_h = os.path.join(buildFolder, '..', 'src', 'config.h')
matepath_config_h = os.path.join(buildFolder, '..', 'matepath', 'src', 'config.h')
notepad4_rc = os.path.join(buildFolder, '..', 'src', 'Notepad4.rc')
matepath_rc = os.path.join(buildFolder, '..', 'matepath', 'src', 'matepath.rc')

backupDir = None
backupMap = {}


def backup_sources():
    global backupDir
    backupDir = os.path.join(tempfile.mkdtemp(prefix='zhhans_hd_'))
    for path in (notepad4_config_h, matepath_config_h, notepad4_rc, matepath_rc):
        target = os.path.join(backupDir, os.path.basename(os.path.dirname(path)) + '_' + os.path.basename(path))
        shutil.copyfile(path, target)
        backupMap[path] = target


def restore_sources():
    if backupDir is None:
        return
    for path, target in backupMap.items():
        shutil.copyfile(target, path)
    shutil.rmtree(backupDir, ignore_errors=True)

config_content = '''#pragma once

#define NP2_ENABLE_CUSTOMIZE_TOOLBAR_LABELS\t\t0
#define NP2_ENABLE_HIDPI_IMAGE_RESOURCE\t\t\t1
#define NP2_ENABLE_DOT_LOG_FEATURE\t\t\t\t0
#define NP2_ENABLE_APP_LOCALIZATION_DLL\t\t\t0
#define NP2_ENABLE_TEST_LOCALIZATION_LAYOUT\t\t0
#define NP2_ENABLE_LOCALIZE_LEXER_NAME\t\t\t1
#define NP2_ENABLE_LOCALIZE_STYLE_NAME\t\t\t1
#define NP2_ENABLE_MD_PREVIEW\t\t\t\t\t1
'''


def restore_resource_include_path(path, matepath):
    with open(path, encoding='utf-8', newline='\n') as fd:
        doc = fd.read()
    if matepath:
        doc = doc.replace('../../matepath/src/', '')
        doc = doc.replace(r'..\\matepath\\', '')
    else:
        doc = doc.replace('../../src/', '')
        doc = doc.replace(r'..\\..\\res', r'..\\res')
    with open(path, 'w', encoding='utf-8', newline='\n') as fd:
        fd.write(doc)


def copy_localized_resources():
    folder = os.path.join(localeDir, 'zh-Hans')
    print('copy zh-Hans resources to src/')
    shutil.copyfile(os.path.join(folder, 'Notepad4.rc'), notepad4_rc)
    restore_resource_include_path(notepad4_rc, False)
    shutil.copyfile(os.path.join(folder, 'matepath.rc'), matepath_rc)
    restore_resource_include_path(matepath_rc, True)


def write_config():
    print('write zh-Hans HD config.h')
    for path in (notepad4_config_h, matepath_config_h):
        with open(path, 'w', encoding='utf-8') as fd:
            fd.write(config_content)


def get_app_version():
    minor = build = reversion = ''
    with open(os.path.join(buildFolder, '..', 'src', 'VersionRev.h'), encoding='utf-8') as fd:
        for line in fd.read().splitlines():
            items = line.split()
            if len(items) > 2:
                key, value = items[1], items[2]
                if key == 'VERSION_MINOR':
                    minor = value
                elif key == 'VERSION_BUILD':
                    build = value
                elif key == 'VERSION_REV':
                    reversion = value
    return f'v{minor}.{build}r{reversion}'


def find_7z_path():
    path = shutil.which('7z.exe')
    if path:
        return path
    import winreg
    for program in ('7-Zip', '7-Zip-Zstandard'):
        try:
            key = winreg.OpenKeyEx(winreg.HKEY_LOCAL_MACHINE, rf"SOFTWARE\{program}", access=winreg.KEY_READ)
            path, _ = winreg.QueryValueEx(key, 'Path')
            winreg.CloseKey(key)
            if path and os.path.isdir(path):
                candidate = os.path.join(path, '7z.exe')
                if os.path.isfile(candidate):
                    return candidate
        except OSError:
            pass
    return '7z.exe'


def make_zip(app_version):
    sevenzip = find_7z_path()
    print('7z path:', sevenzip)
    outDir = os.path.join(buildFolder, 'bin', 'Release', 'AVX2')
    notepad4_exe = os.path.join(outDir, 'Notepad4.exe')
    matepath_exe = os.path.join(outDir, 'matepath.exe')
    if not (os.path.isfile(notepad4_exe) and os.path.isfile(matepath_exe)):
        print('build failure, exe not found:', outDir)
        sys.exit(1)

    zipDir = os.path.join(buildFolder, 'temp_zip_dir')
    if os.path.exists(zipDir):
        shutil.rmtree(zipDir)
    os.makedirs(zipDir)
    for path in ('../License.txt', '../doc/Notepad4.ini', '../doc/Notepad4 DarkTheme.ini',
                 '../matepath/doc/matepath.ini'):
        src = os.path.join(buildFolder, path)
        target = os.path.join(zipDir, os.path.basename(path))
        if target.endswith('.ini'):
            target += '-default'
        shutil.copyfile(src, target)
    shutil.copyfile(notepad4_exe, os.path.join(zipDir, 'Notepad4.exe'))
    shutil.copyfile(matepath_exe, os.path.join(zipDir, 'matepath.exe'))

    # Markdown preview assets, loaded from the Notepad4.exe folder via the
    # "appassets" virtual host in WebView2.
    jsDir = os.path.join(buildFolder, '..', 'webview2', 'js')
    for js in ('marked.min.js', 'mermaid.min.js'):
        src = os.path.join(jsDir, js)
        if os.path.isfile(src):
            shutil.copyfile(src, os.path.join(zipDir, js))

    name = f'Notepad4_HD_zh-Hans_AVX2_{app_version}.zip'
    out = os.path.join(buildFolder, name)
    if os.path.exists(out):
        os.remove(out)
    print('make:', name)
    subprocess.run([sevenzip, 'a', '-tzip', '-mx=9', out], cwd=zipDir, stdout=subprocess.DEVNULL, check=True)
    shutil.rmtree(zipDir)


def main():
    app_version = get_app_version()
    print('app version:', app_version)
    backup_sources()
    try:
        write_config()
        copy_localized_resources()

        command = 'call build.bat Build AVX2 Release 1'
        print(f'run: {command} @ {projectDir}')
        os.chdir(projectDir)
        result = os.popen(command).read()
        if '[ERROR]' in result:
            print('build failure')
            sys.exit(1)
        os.chdir(buildFolder)
        make_zip(app_version)
        print('done.')
    finally:
        restore_sources()


if __name__ == '__main__':
    main()
