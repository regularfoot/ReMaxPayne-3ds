"""Resolve the game's custom idle slots, per-skin overrides and animation messages."""
import re

SLOTS = tuple(f'charanim_{prefix}customidle{i}' for prefix in ('','w_','2_','w_2_') for i in range(1,5))


def definitions(text, *, all_slots=False):
    text=re.sub(r'//[^\r\n]*|/\*.*?\*/','',text,flags=re.S)
    definitions={}
    for block in re.split(r'(?=\[(?:Animation|Revert)\])',text,flags=re.I):
        anim=re.match(r'\[Animation\]\s*Index\s*=\s*(\w+)\s*;\s*Filename\s*=\s*"?(?:anim\\)?([^";]+?)"?\s*;',block,re.I)
        if anim and (all_slots or anim[1].lower() in SLOTS):
            cues=[(int(frame),line) for frame,line in re.findall(r'\[Message\]\s*Frame\s*=\s*(\d+)\s*;\s*String\s*=\s*"([^\r\n]*)"\s*;',block,re.I)]
            definitions[anim[1].lower()]=(anim[2].lower().removesuffix('.kf2'),cues)
        ref=re.match(r'\[Revert\]\s*From\s*=\s*(\w+)\s*;\s*To\s*=\s*(\w+)\s*;',block,re.I)
        if ref and (all_slots or ref[1].lower() in SLOTS):definitions[ref[1].lower()]=ref[2].lower()
    return definitions


def slots(defaults,skin):
    table={**defaults,**definitions(skin)}
    result=[]
    for name in SLOTS:
        value=table.get(name)
        seen=set()
        while isinstance(value,str) and value not in seen:
            seen.add(value);value=table.get(value)
        result.append(value if isinstance(value,tuple) else ('',[]))
    return result
