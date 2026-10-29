#!/usr/bin/env python3
"""Fetch C64 dependencies from the configured fork; keep downloaded assets out of Git.
Optional offline inputs: C64_FRANK_ARCHIVE, C64_ELITE_FILE. Both are verified.
"""
import hashlib,json,os,pathlib,shutil,subprocess,tempfile,urllib.request,zipfile
ROOT=pathlib.Path(__file__).resolve().parents[1]
CONFIG=json.loads((ROOT/'third_party/c64-sources.json').read_text())
def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def valid_core(path):
 return (path/'LICENSE').is_file() and digest(path/'LICENSE')==CONFIG['frank_license_sha256'] and all((path/'src'/name).is_file() and digest(path/'src'/name)==sha for name,sha in CONFIG['frank_files'].items())
def download(url,path):
 subprocess.run(['curl','-fL','--retry','3','--connect-timeout','30','--max-time','180',url,'-o',str(path)],check=True)
def main():
 deps=ROOT/'deps';deps.mkdir(exist_ok=True);dest=deps/'frank-c64'
 if not valid_core(dest):
  if dest.exists():raise RuntimeError('deps/frank-c64 differs from the pinned source. Move it aside before fetching; local changes are not overwritten.')
  with tempfile.TemporaryDirectory(dir=deps,prefix='c64-fetch-') as t:
   temp=pathlib.Path(t);archive=temp/'source.zip';offline=os.environ.get('C64_FRANK_ARCHIVE')
   if offline:shutil.copyfile(offline,archive)
   else:
    repo=CONFIG['frank_repository'].removesuffix('.git').rstrip('/')
    if not repo.startswith('https://github.com/'):raise RuntimeError('frank_repository must be an HTTPS GitHub repository')
    download(repo+'/archive/'+CONFIG['frank_revision']+'.zip',archive)
   extracted=temp/'source';extracted.mkdir()
   with zipfile.ZipFile(archive) as z:
    for item in z.infolist():
     parts=pathlib.PurePosixPath(item.filename).parts
     if len(parts)<2 or item.is_dir():continue
     rel=pathlib.Path(*parts[1:])
     if '..' in rel.parts or rel.is_absolute():raise RuntimeError('Unsafe archive path')
     target=extracted/rel;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(z.read(item))
   subprocess.run(['git','init','-q',str(extracted)],check=True)
   subprocess.run(['git','apply','--check',str(ROOT/'third_party/frank-c64-presto.patch')],cwd=extracted,check=True)
   subprocess.run(['git','apply',str(ROOT/'third_party/frank-c64-presto.patch')],cwd=extracted,check=True)
   if not valid_core(extracted):raise RuntimeError('FRANK source checksum mismatch after applying the Presto patch')
   shutil.rmtree(extracted/'.git')
   shutil.move(str(extracted),dest)
 disk=ROOT/'discs/elite-commodore-64-gma86-pal.d64';disk.parent.mkdir(exist_ok=True)
 if disk.exists():
  if digest(disk)!=CONFIG['elite_sha256']:raise RuntimeError('Existing C64 Elite disk differs from the pinned image; it has not been overwritten.')
 else:
  with tempfile.TemporaryDirectory(dir=disk.parent,prefix='c64-disc-') as t:
   pending=pathlib.Path(t)/'elite.d64';offline=os.environ.get('C64_ELITE_FILE')
   if offline:shutil.copyfile(offline,pending)
   else:download(CONFIG['elite_url'],pending)
   if digest(pending)!=CONFIG['elite_sha256']:raise RuntimeError('Elite download checksum mismatch')
   shutil.move(str(pending),disk)
 print('C64 dependencies verified: FRANK fork + Presto patch, Elite GMA86 PAL')
if __name__=='__main__':
 try:main()
 except Exception as e:raise SystemExit('C64 dependency error: '+str(e)+'\nFork rh1tech/frank-c64 first and configure third_party/c64-sources.json; see docs/c64-pr-build.md.')
