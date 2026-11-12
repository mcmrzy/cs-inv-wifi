"""Generate a build-private PIN key header; never execute or print legacy secrets."""
import argparse
import ast
import os
from pathlib import Path
import sys

class SecretError(ValueError):
    pass

def legacy_secret(source):
    values={}
    def read(node):
        if isinstance(node,ast.Constant) and isinstance(node.value,(str,bytes)):
            return node.value
        if isinstance(node,ast.Name) and node.id in values:
            return values[node.id]
        if isinstance(node,ast.BinOp) and isinstance(node.op,ast.Add):
            left,right=read(node.left),read(node.right)
            if type(left) is type(right):return left+right
        if (isinstance(node,ast.Call) and isinstance(node.func,ast.Attribute) and
            node.func.attr=='encode' and len(node.args)<=1 and not node.keywords):
            value=read(node.func.value)
            encoding=read(node.args[0]) if node.args else 'utf-8'
            if isinstance(value,str) and encoding=='utf-8':return value.encode('utf-8')
        raise SecretError('Unsupported legacy PIN configuration')
    try:
        tree=ast.parse(Path(source).read_text(encoding='utf-8'))
        for statement in tree.body:
            if not isinstance(statement,ast.Assign) or len(statement.targets)!=1:continue
            target=statement.targets[0]
            if not isinstance(target,ast.Name):continue
            try:values[target.id]=read(statement.value)
            except SecretError:
                if target.id=='SECRET':raise
            if target.id=='SECRET':return values['SECRET']
    except (OSError,SyntaxError,UnicodeError) as error:
        raise SecretError('Legacy PIN configuration unavailable') from error
    raise SecretError('Legacy PIN configuration unavailable')

def generate(output,source,environ=None):
    environ=os.environ if environ is None else environ
    if 'PRODUCT_SECRET' in environ:
        secret=environ['PRODUCT_SECRET'].encode('utf-8')
        origin='environment'
    else:
        secret=legacy_secret(source)
        origin='legacy-vectors'
    if isinstance(secret,str):secret=secret.encode('utf-8')
    if not isinstance(secret,bytes) or not 8<=len(secret)<=256 or b'\0' in secret:
        raise SecretError('Invalid PIN configuration')
    header=('/* Generated build-private configuration. Do not copy into release source. */\n'
            '#pragma once\nstatic const unsigned char panel_pairing_secret[]={'+
            ','.join(f'0x{value:02x}' for value in secret)+'};\n')
    output=Path(output)
    output.parent.mkdir(parents=True,exist_ok=True)
    if not output.exists() or output.read_text(encoding='utf-8')!=header:
        temporary=output.with_suffix(output.suffix+'.tmp')
        temporary.write_text(header,encoding='utf-8')
        temporary.replace(output)
    return origin

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',required=True)
    parser.add_argument('--legacy-source',type=Path,default=Path(__file__).resolve().parents[2]/'tools/pin_test_vectors.py')
    args=parser.parse_args()
    try:
        source=generate(args.output,args.legacy_source)
    except (SecretError,OSError,UnicodeError):
        print('PIN configuration unavailable or invalid; no key material logged',file=sys.stderr)
        return 1
    print('PIN build configuration ready ('+source+')')
    return 0

if __name__=='__main__':
    raise SystemExit(main())
