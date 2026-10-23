"""Strict, public-only, bounded build-time package trust policy."""
import hashlib
import json
from pathlib import Path

MAX_KEYS = 8
USAGE = {'module': 1, 'app': 2}


def validate(policy):
    if not isinstance(policy, dict) or set(policy) != {'version', 'keys'} or type(policy['version']) is not int or policy['version'] != 1:
        raise ValueError('Expected trust policy version 1 with keys')
    keys = policy['keys']
    if not isinstance(keys, list) or not 1 <= len(keys) <= MAX_KEYS:
        raise ValueError('Trust policy requires 1 to 8 keys')
    seen = set()
    for key in keys:
        if not isinstance(key, dict) or set(key) != {'public_key', 'usage', 'revoked'}:
            raise ValueError('Trust entry requires public_key, usage and revoked')
        public = key['public_key']
        if not isinstance(public, str) or len(public) != 64 or any(c not in '0123456789abcdef' for c in public):
            raise ValueError('Public key must be 32 bytes of lowercase hexadecimal')
        if public in seen:
            raise ValueError('Duplicate public key')
        seen.add(public)
        usage = key['usage']
        if not isinstance(usage, list) or not usage or any(not isinstance(x, str) or x not in USAGE for x in usage) or len(set(usage)) != len(usage):
            raise ValueError('Key usage must contain unique module/app values')
        if type(key['revoked']) is not bool:
            raise ValueError('revoked must be boolean')
    return policy


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('Duplicate JSON field: ' + key)
        result[key] = value
    return result


def load(path):
    with Path(path).open('rb') as stream:
        raw = stream.read(16385)
    if len(raw) > 16384:
        raise ValueError('Trust policy exceeds 16 KiB')
    return validate(json.loads(raw, object_pairs_hook=unique_object))


def development(module_public, application_public):
    if module_public == application_public:
        raise ValueError('Module and application signing keys must be distinct')
    return validate({'version': 1, 'keys': [
        {'public_key': module_public.hex(), 'usage': ['module'], 'revoked': False},
        {'public_key': application_public.hex(), 'usage': ['app'], 'revoked': False}]})


def header(policy):
    validate(policy)
    rows = []
    for key in policy['keys']:
        public = ','.join(map(str, bytes.fromhex(key['public_key'])))
        rows.append('{{' + public + '},' + str(sum(USAGE[x] for x in key['usage'])) + ',' + str(int(key['revoked'])) + '}')
    return '#include "package.h"\nstatic const SfTrustKey sf_trust_keys[]={' + ','.join(rows) + '};\nstatic const SfTrustStore sf_trust_store={sf_trust_keys,sizeof(sf_trust_keys)/sizeof(*sf_trust_keys)};\n'


def describe(policy):
    validate(policy)
    return {'version': 1, 'keys': [dict(key, key_id=hashlib.sha256(bytes.fromhex(key['public_key'])).hexdigest()) for key in policy['keys']]}
