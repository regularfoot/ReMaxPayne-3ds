"""Pack the retail Xbox transition table and its authored action IDs."""
import argparse,hashlib,json,re,struct
from pathlib import Path
from pack_native import member
ROOT=Path(__file__).resolve().parents[1]
XBE_SHA='212c47d25f96b68996d085abf93cb43464b8cf34f156081b4919866a2be74102'
def encode(disc,folder):
    if hashlib.sha256((disc/'default.xbe').read_bytes()).hexdigest()!=XBE_SHA:raise ValueError('Transition oracle belongs to a different Xbox executable')
    oracle=(ROOT/'tests/fixtures/xbox_transition_rules.bin').read_bytes()
    tag,rules,copy,phase=struct.unpack_from('<4sIII',oracle)
    if (tag,rules,copy,phase)!=(b'XTR1',447,32,144) or len(oracle)!=16+24*rules+4*(copy+phase):raise ValueError('Invalid retail transition oracle')
    text=member(disc/'english/x_headers.ras','data/database/characteranimid.h').decode('latin1')
    ids={k.lower().removeprefix('charanim_'):int(v,0) for k,v in re.findall(r'#define\s+(CHARANIM_\w+)\s+(0x[0-9a-fA-F]+|[0-9]+)',text)}
    index=(folder/'poses.idx').read_bytes();actions=struct.unpack_from('<I',index,8)[0];mapping=[]
    for i in range(actions):
        key=index[16+64*i:16+64*(i+1)].split(b'\0')[0].decode();value=ids[key]
        if not 0<=value<rules:raise ValueError('Missing retail timing for '+key)
        mapping.append(value)
    return struct.pack('<4sIIII',b'TRN1',rules,actions,copy,phase)+oracle[16:16+24*rules]+struct.pack('<'+'H'*actions,*mapping)+oracle[16+24*rules:]
def pack(disc,folder):
    data=encode(disc,folder);(folder/'transitions.bin').write_bytes(data);print('Packed 447 Xbox transition rules, 176 phase/time pairs and every authored action')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('disc',type=Path);p.add_argument('folder',type=Path);a=p.parse_args();pack(a.disc,a.folder)
