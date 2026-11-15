"""Verify test build packaging, RAM budget and absence of Bluetooth flash storage.
Usage: python3 tests/bluetooth/firmware_test.py <build-root> <arm-none-eabi-nm>
"""
import importlib.util, pathlib, subprocess, sys, struct, json
root=pathlib.Path(__file__).resolve().parents[2]
build=pathlib.Path(sys.argv[1]); nm=sys.argv[2]
spec=importlib.util.spec_from_file_location('uf2',root/'tools/dual_boot_uf2.py')
u=importlib.util.module_from_spec(spec);spec.loader.exec_module(u)
expected={}
for machine,name,offset in [('launcher','presto_launcher',0),('master','presto_master',32768),('beeb','presto_beebjit',4128*1024)]:
    for address,payload in u.blocks(build/machine/(name+'.uf2')):
        assert address+offset not in expected
        expected[address+offset]=payload
    if machine=='launcher':continue
    text=subprocess.check_output([nm,'-C',str(build/machine/(name+'.elf'))],text=True)
    symbols={parts[2]:int(parts[0],16) for line in text.splitlines() if len(parts:=line.split(maxsplit=2))==3}
    elf=(build/machine/(name+'.elf')).read_bytes()
    phoff=struct.unpack_from('<I',elf,28)[0]; phsize,phnum=struct.unpack_from('<HH',elf,42)
    address=symbols['s_layout']
    table=None
    for i in range(phnum):
        kind,offset,vaddr,paddr,filesz,memsz,flags,align=struct.unpack_from('<8I',elf,phoff+i*phsize)
        if kind==1 and vaddr<=address and address+48<=vaddr+filesz:
            table=struct.unpack_from('<12I',elf,offset+address-vaddr)
    assert table is not None
    regions=list(zip(table[::2],table[1::2]));banks=regions[4:]
    assert banks==[(0xf00000,4096),(0xf01000,4096)],banks
    occupied=regions[:4]+[(regions[0][0]-256*1024,regions[0][1]),(regions[1][0]-256*1024,regions[1][1])]
    partitions=json.loads((root/'src/launcher/partitions.json').read_text())['partitions']
    occupied += [(u.size_k(p['start']),u.size_k(p['size'])) for p in partitions]
    for a,n in banks:
        assert all(a+n<=b or b+m<=a for b,m in occupied)
    assert 'btstack_cyw43_init' in symbols
    assert 'btstack_link_key_db_memory_instance' in symbols
    assert not any('pico_flash_bank' in s or 'btstack_link_key_db_tlv' in s for s in symbols)
    cache=(build/machine/'CMakeCache.txt').read_text();assert 'BLUETOOTH:BOOL=ON' in cache
    if 'BT_KEYBOARD_CLASSIC:BOOL=ON' in cache:
        assert 'keyboard_host_frame' in symbols
    if 'BT_KEYBOARD_DISCOVERY:BOOL=ON' in cache:
        assert 'keyboard_discovery_start' in symbols
    gap=symbols['__HeapLimit']-symbols['__end__']
    assert gap>=32768,(machine,gap)
    print(machine,'static headroom:',gap,'bytes (not live free RAM)')
combined=u.blocks(build/'presto_dual_boot.uf2')
assert len(combined)==len(expected) and dict(combined)==expected
print('All targets use reserved nonoverlapping controller banks and exclude SDK flash TLV; combined UF2 exactly matches',len(expected),'payload blocks')
