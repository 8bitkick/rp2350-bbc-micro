"""Offline dependency extraction, patching, reuse and modified-file protection.
Usage: python3 tests/c64/dependency_test.py /path/frank.zip /path/elite.d64
"""
from pathlib import Path
import os,subprocess,sys,tempfile,shutil
root=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as td:
 repo=Path(td);(repo/'scripts').mkdir();shutil.copy2(root/'scripts/fetch-c64.py',repo/'scripts/fetch-c64.py');shutil.copytree(root/'third_party',repo/'third_party')
 subprocess.run(['git','init','-q',str(repo)],check=True)
 env=dict(os.environ,C64_FRANK_ARCHIVE=str(Path(sys.argv[1]).resolve()),C64_ELITE_FILE=str(Path(sys.argv[2]).resolve()))
 cmd=[sys.executable,str(repo/'scripts/fetch-c64.py')]
 for _ in range(2):subprocess.run(cmd,env=env,check=True)
 edited=repo/'deps/frank-c64/src/VIC.cpp';edited.write_text('local work\n')
 result=subprocess.run(cmd,env=env,capture_output=True,text=True)
 assert result.returncode!=0 and edited.read_text()=='local work\n'
print('Dependency extraction inside a Git checkout, checksums, reuse and local-edit protection passed')
