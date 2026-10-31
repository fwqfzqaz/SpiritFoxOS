"""Capture or compare the current x86_64 fixed-width header ABI inventory."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
FORMAT='spiritfoxos-x86_64-abi-snapshot-v1'
REFERENCE=ROOT/'abi/x86_64-current.json'
MAX_BYTES=256*1024
MAX_ENTRIES=2048


def unique(pairs):
    result={}
    for key,value in pairs:
        if key in result:raise ValueError('Duplicate ABI field: '+repr(key))
        result[key]=value
    return result


def validate(snapshot):
    if type(snapshot) is not dict or set(snapshot)!={'format','values'} or snapshot['format']!=FORMAT:
        raise ValueError('Expected '+FORMAT+' with values')
    values=snapshot['values']
    if type(values) is not dict or not 1<=len(values)<=MAX_ENTRIES:
        raise ValueError('Expected 1..2048 ABI entries')
    for name,value in values.items():
        if not isinstance(name,str) or re.fullmatch(r'[A-Za-z][A-Za-z0-9_.]{0,127}',name) is None:
            raise ValueError('Invalid ABI entry name')
        if type(value) is not int or not -(2**63)<=value<=2**64-1:
            raise ValueError('ABI entries must be bounded integers: '+name)
    return snapshot


def decode(raw):
    if len(raw)>MAX_BYTES:raise ValueError('ABI snapshot exceeds 256 KiB')
    try:return validate(json.loads(raw.decode('utf-8'),object_pairs_hook=unique))
    except RecursionError:raise ValueError('Invalid nested ABI snapshot') from None


def load(path):
    with Path(path).open('rb') as stream:return decode(stream.read(MAX_BYTES+1))


def difference(before,after):
    a=validate(before)['values'];b=validate(after)['values']
    return {'added':{k:b[k] for k in sorted(set(b)-set(a))},
            'removed':{k:a[k] for k in sorted(set(a)-set(b))},
            'changed':{k:{'before':a[k],'after':b[k]} for k in sorted(set(a)&set(b)) if a[k]!=b[k]}}


def reviewed_additions(entries):
    if len(entries)>MAX_ENTRIES:raise ValueError('Too many reviewed ABI additions')
    result={}
    for entry in entries:
        if len(entry)>151 or '=' not in entry:raise ValueError('Expected constant.NAME=DECIMAL')
        name,number=entry.split('=',1)
        if re.fullmatch(r'constant\.[A-Za-z][A-Za-z0-9_]{0,118}',name) is None:
            raise ValueError('Only named constant additions can be reviewed: '+name)
        if len(number)>21 or re.fullmatch(r'-?(0|[1-9][0-9]*)',number) is None:
            raise ValueError('Expected canonical decimal ABI value: '+name)
        value=int(number)
        if str(value)!=number or not -(2**63)<=value<=2**64-1:
            raise ValueError('Reviewed ABI value out of range or noncanonical: '+name)
        if name in result:raise ValueError('Duplicate reviewed ABI addition: '+name)
        result[name]=value
    return result


def check_additions(before,after,entries):
    review=reviewed_additions(entries);report=difference(before,after);added=report['added']
    report['unreviewed_added']={k:added[k] for k in sorted(set(added)-set(review))}
    report['unused_reviews']={k:review[k] for k in sorted(set(review)-set(added))}
    report['review_value_mismatches']={k:{'reviewed':review[k],'actual':added[k]}
        for k in sorted(set(review)&set(added)) if review[k]!=added[k]}
    report['baseline_preserved']=not report['removed'] and not report['changed']
    report['reviewed_additions_match']=report['baseline_preserved'] and not any(
        report[k] for k in ['unreviewed_added','unused_reviews','review_value_mismatches'])
    return report


def capture():
    # Lazy import: checking saved reports needs neither Zig nor a build tree.
    if not (ROOT/'tools/build.py').is_file() or not (ROOT/'tools/abi_snapshot.c').is_file():
        raise ValueError('Capture requires a source checkout and workspace toolchain')
    import build
    executable=ROOT/('build/abi-snapshot'+build.EXE)
    build.run(['-target',build.HOST,'-std=c11','-O2','-Wall','-Wextra','-Werror',
               '-Iinclude','tools/abi_snapshot.c','-o',str(executable)])
    result=subprocess.run([str(executable)],check=True,stdout=subprocess.PIPE)
    return decode(result.stdout)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    sub=parser.add_subparsers(dest='command',required=True)
    p=sub.add_parser('capture',help='compile the fixed catalog against current shared headers')
    p.add_argument('--out',type=Path,default=ROOT/'build/abi-current.json')
    p=sub.add_parser('check',help='compare a saved capture with the reviewed reference')
    p.add_argument('file',type=Path)
    p.add_argument('--reference',type=Path,default=REFERENCE)
    p=sub.add_parser('diff',help='report numeric/layout changes; additions also require review')
    p.add_argument('before',type=Path);p.add_argument('after',type=Path)
    p=sub.add_parser('check-additions',help='preserve every baseline entry and require exact reviewed constant additions')
    p.add_argument('before',type=Path);p.add_argument('after',type=Path)
    p.add_argument('--allow-add',action='append',default=[],metavar='constant.NAME=DECIMAL')
    args=parser.parse_args()
    if args.command=='capture':
        out=args.out.resolve()
        snapshot=capture()
        text=json.dumps(snapshot,indent=2,sort_keys=True)+'\n'
        out.write_text(text,encoding='utf-8',newline='\n')
        print(json.dumps({'file':str(out),'entries':len(snapshot['values']),
                          'sha256':hashlib.sha256(text.encode()).hexdigest()}))
        return 0
    before=load(args.reference if args.command=='check' else args.before)
    after=load(args.file if args.command=='check' else args.after)
    if args.command=='check-additions':
        report=check_additions(before,after,args.allow_add)
        print(json.dumps(report,indent=2,sort_keys=True))
        return 0 if report['reviewed_additions_match'] else 1
    report=difference(before,after)
    matches=not any(report.values())
    print(json.dumps(dict(report,layout_matches=matches),indent=2,sort_keys=True))
    return 0 if matches else 1


if __name__=='__main__':
    try:raise SystemExit(main())
    except (OSError,ValueError,RuntimeError,subprocess.CalledProcessError) as error:
        raise SystemExit('abi: '+str(error)) from None
