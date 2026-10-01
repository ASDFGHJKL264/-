"""Build release executables, deploy Qt, and create a portable ZIP and Windows SFX installer.

Requires an existing Qt/MinGW/CMake/Ninja installation plus 7-Zip and WinRAR.
Creates a new dist folder for each run; never includes local databases, logs or config.ini.
"""
from pathlib import Path
import argparse
import datetime
import hashlib
import json
import os
import shutil
import subprocess
import zipfile

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qt-prefix', default='D:/Qt6/6.10.3/mingw_64')
    parser.add_argument('--compiler-bin', default='D:/Qt6/Tools/mingw1310_64/bin')
    parser.add_argument('--cmake', default='D:/Qt6/Tools/CMake_64/bin/cmake.exe')
    parser.add_argument('--ninja', default='D:/Qt6/Tools/Ninja/ninja.exe')
    parser.add_argument('--sevenzip', default='D:/7-zip/7z.exe')
    parser.add_argument('--rar', default='D:/WinRAR/Rar.exe')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    product = json.loads((root/'packaging/product.json').read_text(encoding='utf-8-sig'))
    qt, compiler = Path(args.qt_prefix), Path(args.compiler_bin)
    deploy = qt/'bin/windeployqt.exe'
    sfx = Path(args.rar).parent/'Default.SFX'
    for tool in [args.cmake,args.ninja,args.sevenzip,args.rar,deploy,sfx,compiler/'g++.exe']:
        if not Path(tool).is_file(): raise FileNotFoundError(tool)
    env = dict(os.environ)
    env['PATH'] = str(compiler)+os.pathsep+str(qt/'bin')+os.pathsep+env.get('PATH','')
    def run(command, cwd=root):
        print('RUN', subprocess.list2cmdline([str(x) for x in command]), flush=True)
        subprocess.run([str(x) for x in command], cwd=cwd, env=env, check=True)
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    output = root/'dist'/('release-'+stamp)
    bundle = output/'installer-content'
    payload = bundle/'payload'
    payload.mkdir(parents=True)
    for app in product['applications']:
        build = root/'build'/('release-publish-'+app['target'])
        run([args.cmake,'-S',root/app['source'],'-B',build,'-G','Ninja',
             '-DCMAKE_MAKE_PROGRAM='+args.ninja,'-DCMAKE_CXX_COMPILER='+str(compiler/'g++.exe'),
             '-DCMAKE_PREFIX_PATH='+str(qt),'-DCMAKE_BUILD_TYPE=Release','-DBUILD_TESTING=OFF'])
        run([args.cmake,'--build',build,'--target',app['target'],'--parallel','4'])
        executable = payload/app['executable']
        shutil.copy2(build/(app['target']+'.exe'),executable)
        run([deploy,'--release','--compiler-runtime','--no-translations','--dir',payload,executable])
    for name in product['documents']:
        shutil.copy2(root/name,payload/Path(name).name)
    (payload/'qt.conf').write_text('[Paths]\nPrefix=.\nPlugins=.\n',encoding='utf-8')
    # Record the source tree, including uncommitted packaging changes, without credentials.
    source_files = subprocess.check_output(['git','ls-files','-z','--cached','--others','--exclude-standard'],cwd=root).decode('utf-8').split('\0')
    hashes = {name:hashlib.sha256((root/name).read_bytes()).hexdigest()
              for name in sorted(set(source_files)) if name and (root/name).is_file()}
    record = {'product':product['name'],'version':product['version'],'builtAt':datetime.datetime.now().isoformat(),
              'baseCommit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=root).decode().strip(),
              'sourceSha256':hashes,'configuration':'Release','qtPrefix':str(qt)}
    (payload/'release-manifest.json').write_text(json.dumps(record,ensure_ascii=False,indent=2),encoding='utf-8')
    digests = {p.relative_to(payload).as_posix():hashlib.sha256(p.read_bytes()).hexdigest()
               for p in sorted(payload.rglob('*')) if p.is_file()}
    (payload/'SHA256.json').write_text(json.dumps(digests,ensure_ascii=False,indent=2),encoding='utf-8')
    shutil.copy2(root/'packaging/install.ps1',bundle/'install.ps1')
    shutil.copy2(root/'packaging/product.json',bundle/'product.json')
    setup = '@echo off\r\npowershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1"\r\n'
    (bundle/'Install.cmd').write_bytes(setup.encode('ascii'))
    archive = output/(product['name']+'_便携版.zip')
    with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED,compresslevel=6) as z:
        for p in sorted(payload.rglob('*')):
            if p.is_file():z.write(p,product['name']+'/'+p.relative_to(payload).as_posix())
    installer = output/(product['name']+'_安装包.exe')
    comment = output/'sfx-comment.txt'
    comment.write_text('; SFX installation commands\nTempMode\nSilent=1\nOverwrite=1\n'
        'Setup=powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File install.ps1\n'
        'Title=Qt Desktop Application Setup\n',encoding='ascii')
    run([args.rar,'a','-r','-m5','-s','-sfx'+str(sfx),'-z'+str(comment),installer,'.'],cwd=bundle)
    run([args.sevenzip,'t',archive])
    run([args.sevenzip,'t',installer])
    result={'output':str(output),'payload':str(payload),'installerContent':str(bundle),'installer':str(installer),'portable':str(archive),
            'sha256':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in [installer,archive]}}
    (output/'artifacts.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    (root/'dist/latest-release.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(result,ensure_ascii=False,indent=2),flush=True)

if __name__=='__main__': main()
