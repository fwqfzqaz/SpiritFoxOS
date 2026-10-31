"""Bounded JSON source reader and the opt-in native-source-v1 author profile.

This validates a fixed repository profile, not arbitrary JSON Schema documents.
Guest signature, usage, revocation, ABI and capability checks remain authoritative.
"""
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
MAX_SOURCE_BYTES = 256*1024
MAX_ENCODED_BYTES = 64*1024
MAX_DEPTH = 16
MAX_NODES = 4096
SCHEMA_PATH = ROOT/'schemas/native-source-v1.schema.json'


def _unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('Duplicate manifest field: '+repr(key))
        result[key] = value
    return result


def _no_float(value):
    raise ValueError('Floating-point/non-finite manifest numbers are unsupported')


def _bounded(value):
    remaining = MAX_NODES
    def visit(item, depth):
        nonlocal remaining
        remaining -= 1
        if remaining < 0 or depth > MAX_DEPTH:
            raise ValueError('Manifest exceeds node or nesting bound')
        if isinstance(item, str):
            if '\0' in item:
                raise ValueError('NUL in manifest text')
            try:
                item.encode('utf-8')
            except UnicodeEncodeError:
                raise ValueError('Invalid Unicode in manifest text') from None
        elif isinstance(item, dict):
            for key, child in item.items():
                if not isinstance(key, str):
                    raise ValueError('Manifest object keys must be text')
                visit(key, depth+1)
                visit(child, depth+1)
        elif isinstance(item, list):
            for child in item:
                visit(child, depth+1)
        elif item is not None and type(item) not in (bool, int):
            raise ValueError('Unsupported manifest value type')
    visit(value, 0)
    if not isinstance(value, dict):
        raise ValueError('Manifest source must be a JSON object')
    return value


def load(path):
    with Path(path).open('rb') as stream:
        raw = stream.read(MAX_SOURCE_BYTES+1)
    if len(raw) > MAX_SOURCE_BYTES:
        raise ValueError('Manifest JSON exceeds 256 KiB source limit')
    try:
        value = json.loads(raw.decode('utf-8'), object_pairs_hook=_unique,
                           parse_float=_no_float, parse_constant=_no_float)
    except RecursionError:
        raise ValueError('Manifest exceeds nesting bound') from None
    return _bounded(value)


def schema():
    return json.loads(SCHEMA_PATH.read_text(encoding='utf-8'))


def _check(value, rule, path):
    kind = rule['type']
    expected = {'object': dict, 'array': list, 'string': str, 'integer': int}[kind]
    if type(value) is not expected:
        raise ValueError(path+': expected '+kind)
    if 'const' in rule and value != rule['const']:
        raise ValueError(path+': unsupported value')
    if 'enum' in rule and value not in rule['enum']:
        raise ValueError(path+': unsupported value')
    if kind == 'object':
        required = set(rule.get('required', []))
        if required-set(value):
            raise ValueError(path+': missing '+repr(sorted(required-set(value))))
        properties = rule['properties']
        if set(value)-set(properties):
            raise ValueError(path+': unknown '+repr(sorted(set(value)-set(properties))))
        for key, child in value.items():
            _check(child, properties[key], path+'.'+key)
    elif kind == 'array':
        if len(value) > rule['maxItems']:
            raise ValueError(path+': too many items')
        # All profile arrays contain strings; validate before set conversion.
        for i, item in enumerate(value):
            _check(item, rule['items'], path+'['+str(i)+']')
        if rule.get('uniqueItems') and len(set(value)) != len(value):
            raise ValueError(path+': duplicate items')
    elif kind == 'string':
        if not rule.get('minLength', 0) <= len(value) <= rule['maxLength']:
            raise ValueError(path+': invalid length')
        if 'pattern' in rule and re.fullmatch(rule['pattern'], value) is None:
            raise ValueError(path+': invalid text')
    else:
        if not rule.get('minimum', value) <= value <= rule.get('maximum', value):
            raise ValueError(path+': out of range')


def validate(value, kind):
    if kind not in ('module', 'app'):
        raise ValueError('Manifest kind must be module or app')
    _bounded(value)
    _check(value, schema()['oneOf'][0 if kind == 'module' else 1], 'manifest')
    return value
