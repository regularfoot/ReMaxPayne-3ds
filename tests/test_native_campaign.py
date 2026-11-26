"""Validate every authored next-level event using the native director, navigation and moving props.

The driver positions the player at mission interactions and applies combat damage directly.
It exercises progression without rendering a frame or measuring controller travel distances.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import os
import subprocess
from tempfile import TemporaryDirectory
from collections import defaultdict,deque
import gzip,json,re
from pathlib import Path
def canon(s):return re.sub(r'\.(do|trigger)$','',s.strip().lower())
def route(root,stage):
 s=json.load(gzip.open(root/(stage+'.json.gz')))
 rev=defaultdict(set);locations={}
 def register(name,obj):
  p=obj.get('properties',obj.get('object_properties',{}));t=p['object_to_room_transform']
  locations[canon(name)]=(p['room_id'],t[3],obj.get('type',-1))
 def links(source,block):
  for m in block['messages']:
   match=re.fullmatch(r'\s*(.*?)\s*->\s*(\w+)\s*\((.*)\)\s*;?\s*',m)
   if not match:continue
   target,op,args=match.groups();target=canon(target);op=op.lower();k=args.split(',')[0].strip(' "').lower()
   if op in ('gm_init','gm_sendendofgamemessages'):rev[('end','','')].add(source)
   if op=='do_animate':
    for ch in ['leave','arrive']:rev[(target,ch,k)].add(source)
   if op in ('fsm_send','fsm_switch'):rev[(target,op,k)].add(source)
   if op=='t_activate':rev[(target,'entity','t_activate')].add(source)
   if op=='c_sendspecial':rev[(target,'special','')].add(source)
   if op in ('c_setinvulnerable','c_setimmortal') and k in ['false','0']:rev[(target,'vulnerable','')].add(source)
   if op=='t_enable' and k in ['true','1']:rev[(target,'enable','')].add(source)
 for x in s['triggers']:register(x['shared_name'],x)
 for x in s['props']:
  n=canon(x['shared_name']);register(n,x)
  for a in x['animations']['animations']:
   for k,ch in [('leaving_first_frame','leave'),('reaching_second_frame','arrive'),('returning_first_frame','return')]:links((n,ch,a['animation_name'].lower()),a[k])
 for x in s['fsms']:
  n=canon(x['shared_name'])
  for k,ch in [('state_switch','fsm_switch'),('string_specific','fsm_send'),('entity_specific','entity')]:
   for e in x[k]['events']:
    key=e['state_name'].lower()
    for block in [e['before'],e['after']]:
     links((n,ch,key),block)
    for a in e['state_specific']['messages']:
     state=a['state_name'].lower()
     branch=(n,ch+'@'+state,key)
     links(branch,a['messages'])
     rev[branch].add((n,ch,key));rev[branch].add((n,'fsm_switch',state))
  for k in ('startup_before','startup_after'):links((n,'start',''),x[k])
 for x in s['characters']:
  n=canon(x['shared_name']);register(n,x)
  for k,ch in [('on_death_before','death'),('on_activate_before','activate'),('on_special_before','special')]:links((n,ch,''),x[k])
 start=('end','','');q=deque([(start,0)]);seen={start};choices=[]
 while q:
  node,steps=q.popleft();n,ch,key=node
  if n in locations and (ch in ('death','activate') or (ch=='entity' and key in ('t_activate','do_charactercollides','do_bulletcollides','do_ondeath'))):
   choices.append((steps,n,ch,key,*locations[n]))
   if ch=='death' and (n,'vulnerable','') not in seen:
    seen.add((n,'vulnerable',''));q.append(((n,'vulnerable',''),steps+2))
   if key=='t_activate' and (n,'enable','') not in seen:
    seen.add((n,'enable',''));q.append(((n,'enable',''),steps+10))
  for before in sorted(rev[node]):
   if before not in seen:seen.add(before);q.append((before,steps+1))
 return sorted(choices)


def verify(source, assets, jobs):
    project = Path(__file__).resolve().parents[1]
    native = project / 'source/native'
    stages = [level['id'] for level in json.loads((source/'levels.json').read_text()) if level['id'] not in ('tutorial','end_combat')]
    if stages[0] != 'startup_level' or stages[-1] != 'p3_l7-3' or len(stages) != 67:
        raise ValueError('Unexpected campaign table')
    with TemporaryDirectory() as tmp:
        folder = Path(tmp)
        (folder/'sdmc:/3ds/maxpayne').mkdir(parents=True)
        (folder/'difficulty.bin').symlink_to((assets/'difficulty.bin').resolve())
        rules = folder/'rules'
        subprocess.run([os.environ.get('CC','cc'), '-std=c11', '-O2', '-I'+str(native),
                        project/'tests/test_native_campaign_rules.c', native/'game_rules.c', native/'save.c',
                        '-lm', '-o', rules], check=True)
        subprocess.run([rules], cwd=folder, check=True)
        print('Chapter limits, unlocks and recovery of timed records verified')
        runner = folder / 'story'
        subprocess.run([os.environ.get('CC','cc'), '-std=c11', '-O2', '-I'+str(native), project/'tests/native_campaign_story.c',
                        native/'director.c', native/'campaign_nav.c', native/'campaign_bounds.c', native/'pose_bank.c', native/'camera_path.c', native/'save.c',
                        '-lm', '-o', runner], check=True)
        def verify_stage(pair):
            stage, following = pair
            inputs = folder / (stage+'.route')
            actions = route(source,stage)
            inputs.write_text(''.join(action[1].replace(' ','|')+' '+(action[3] or action[2])+'\n' for action in actions))
            result = subprocess.run([runner,assets,stage,inputs],check=True,capture_output=True,text=True,timeout=120)
            outcome = result.stdout.strip().split('\t')
            if outcome != [stage, following]:
                raise ValueError(f'{stage} expected {following}, observed {outcome}')
            return f'{stage} -> {following}'
        with ThreadPoolExecutor(max_workers=jobs) as pool:
            for progress in pool.map(verify_stage,zip(stages,stages[1:]+['CREDITS'])):
                print(progress,flush=True)
        tutorial = folder/'tutorial.route'
        tutorial.write_text('')
        result = subprocess.run([runner,assets,'tutorial',tutorial],check=True,capture_output=True,text=True,timeout=120)
        if result.stdout.strip()!='tutorial\tTRAINING COMPLETE':
            raise ValueError(result.stderr or result.stdout)
        print('Original tutorial, repeat encounter and secret room verified')
        subprocess.run([runner,assets,'p3_l3-2',folder/'p3_l3-2.route','--lose-escort'],
                       check=True,capture_output=True,timeout=120)
        print('Scientist escort failure camera returns to the checkpoint')
        subprocess.run([runner,assets,'p1_l1-1',folder/'p1_l1-1.route','--lockers'],
                       check=True,capture_output=True,timeout=120)
        print('A single press opens the nearest subway locker and preserves the next interaction')
        subprocess.run([runner,assets,'p1_l1-1',folder/'p1_l1-1.route','--wounds'],
                       check=True,capture_output=True,timeout=120)
        print('Subway enemy switches to wounded animations on damage and recovers when healed')
        subprocess.run([runner,assets,'p1_l1b-1',folder/'p1_l1b-1.route','--rats'],
                       check=True,capture_output=True,timeout=120)
        print('Subway rats wait for a scripted opponent and arm for the authored fight')
        for stage in stages:
            if stage=='p1_l1b-1':
                continue
            with gzip.open(source/(stage+'.json.gz')) as level:
                characters=json.load(level)['characters']
            if any(actor['character_name'].lower()=='rat' for actor in characters):
                subprocess.run([runner,assets,stage,folder/(stage+'.route'),'--passive-rats'],
                               check=True,capture_output=True,timeout=120)
                print(f'{stage}: ambient rats remain passive on activation')
        subprocess.run([runner,assets,'p1_l2-3',folder/'p1_l2-3.route','--scripted-combat'],
                       check=True,capture_output=True,timeout=120)
        print('The wounded junkie waits for his scripted opponent and retargets after the opponent dies')
    print(f'{len(stages)} campaign transitions verified')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source',type=Path)
    parser.add_argument('assets',type=Path)
    parser.add_argument('--jobs',type=int,default=4)
    a=parser.parse_args()
    verify(a.source,a.assets,a.jobs)
