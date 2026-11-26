"""Read character movement distances and combat parameters from the Xbox skin database."""
import math
import re
import struct

FIELDS = ('MaximumHealth','WoundedHealth','VisualPerceivingRadius','GeneralPerceivingRadius',
          'ActivationReactionTime','ShootingCone','TurnLeftRightSpeed','AimingSpeedCone',
          'AimingSpeed','ShootingFrequencyMultiplier')

def clean(text):
    return re.sub(r'//[^\r\n]*|/\*.*?\*/','',text,flags=re.S)

def constants(text):
    return dict(re.findall(r'^\s*#define\s+(\w+)\s+([-+.\w]+)',clean(text),re.M))

def number(expr, names):
    expr=expr.strip().replace(' ','').replace('\t','')
    if expr.startswith('-'):return -number(expr[1:],names)
    return float(names.get(expr,expr))

def skin_row(source,names):
    fields=dict(re.findall(r'\b(\w+)\s*=\s*([^;{}]+);',clean(source)))
    values=[number(fields[k],names) for k in FIELDS]
    return struct.pack('<9f',values[1]/max(1,values[0]),*values[2:])


def movements(text,names):
    result={}
    for block in re.split(r'(?=\[(?:Animation|Revert)\])',clean(text),flags=re.I):
        m=re.match(r'\[Animation\]\s*Index\s*=\s*CHARANIM_(\w+)',block,re.I)
        if m:
            end=re.search(r'\[Movement\]\s*EndPosition\s*=\s*\(([^)]*)\)',block,re.I)
            key=m[1].lower()
            result[key]=math.dist([number(x,names) for x in end[1].split(',')],(0,0,0)) if end else 0.
        ref=re.match(r'\[Revert\]\s*From\s*=\s*CHARANIM_(\w+)\s*;\s*To\s*=\s*CHARANIM_(\w+)',block,re.I)
        if ref:result[ref[1].lower()]=ref[2].lower()
    return result

def write_rules(folder,actions,characters,weapons):
    rows=[];travel=[]
    for row,moves in characters:
        rows.append(row)
        for name in actions:
            seen=set()
            while isinstance(name,str) and name not in seen:
                seen.add(name);name=moves.get(name,0.)
            if isinstance(name,str):raise ValueError(f'Cyclic animation alias {name}')
            travel.append(name)
    data=struct.pack('<4sII',b'SKN2',len(rows),len(actions))+b''.join(rows)+struct.pack('<%df'%len(travel),*travel)
    data+=struct.pack('<I',len(weapons))+b''.join(weapons)
    dest=folder/'skin_rules.bin';tmp=dest.with_suffix('.partial');tmp.write_bytes(data);tmp.replace(dest)
