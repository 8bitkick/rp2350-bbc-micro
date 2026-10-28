"""Check slot placement, overflow rejection and mandatory C64 packaging."""
import importlib.util,json,pathlib,subprocess,sys,tempfile
root=pathlib.Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('packer',root/'tools/dual_boot_uf2.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
with tempfile.TemporaryDirectory() as td:
 t=pathlib.Path(td)
 for i,n in enumerate(('launcher','master','beeb','c64')):m.write(t/(n+'.uf2'),[(m.FLASH,bytes([i])*256)])
 args=[sys.executable,str(root/'tools/dual_boot_uf2.py'),str(root/'src/launcher/partitions.json'),*[str(t/(n+'.uf2')) for n in ('launcher','master','beeb')]]
 assert subprocess.run(args+[str(t/'out.uf2')],capture_output=True).returncode!=0
 subprocess.run(args+['--c64',str(t/'c64.uf2'),str(t/'out.uf2')],check=True,capture_output=True)
 blocks=m.blocks(t/'out.uf2');assert [a-m.FLASH for a,b in blocks]==[0,32*1024,4128*1024,8224*1024]
 assert [b[0] for a,b in blocks]==[0,1,2,3]
 m.write(t/'c64.uf2',[(m.FLASH,b'X'*256),(m.FLASH+4096*1024,b'Y'*256)])
 assert subprocess.run(args+['--c64',str(t/'c64.uf2'),str(t/'out.uf2')],capture_output=True).returncode!=0
 try:m.write(t/'bad.uf2',[(m.FLASH,b'X'*256),(m.FLASH,b'Y'*256)])
 except SystemExit:pass
 else:raise AssertionError('overlap accepted')
print('UF2 slot placement and bounds: passed')
