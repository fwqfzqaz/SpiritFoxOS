"""Exercise the actual Persist verify release gate with synthetic records."""
import ast
import copy
import hashlib
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parents[2]
tree = ast.parse((ROOT / 'tools/package_release.py').read_text(encoding='utf-8'))


def assignment(name):
    return next(n for n in tree.body if isinstance(n, ast.Assign) and len(n.targets) == 1 and
                isinstance(n.targets[0], ast.Name) and n.targets[0].id == name)


def module(nodes, filename='actual-package-release-verify-AST'):
    return compile(ast.fix_missing_locations(ast.Module(body=nodes, type_ignores=[])), filename, 'exec')


terminal_sources_node = assignment('terminal_source_paths')
terminal_source_hash_node = assignment('terminal_source_sha256')
artifact_paths_node = assignment('terminal_artifact_paths')
artifact_hash_node = assignment('terminal_artifact_sha256')
verify_paths_node = assignment('verify_source_paths')
verify_hash_node = assignment('verify_source_sha256')
guest_node = assignment('verify_guest')
cases_node = assignment('verify_cases')
validator = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'verify_case_valid')
release_if = next(n for n in tree.body if isinstance(n, ast.If) and
                  any(isinstance(x, ast.Name) and x.id == 'verify_guest' for x in ast.walk(n.test)))
gate_code = module([terminal_sources_node, terminal_source_hash_node, artifact_paths_node,
                    artifact_hash_node, verify_paths_node, verify_hash_node, guest_node,
                    cases_node, validator, release_if])

path_context = {}
exec(module([terminal_sources_node, verify_paths_node], 'actual-verify-source-path-AST'), path_context)
source_paths = path_context['verify_source_paths']
artifact_paths = ast.literal_eval(artifact_paths_node.value)
checks = 0

with tempfile.TemporaryDirectory(prefix='verify-evidence-', dir=ROOT / 'build') as temporary:
    fake_root = Path(temporary)
    for i, path in enumerate(source_paths):
        target = fake_root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(('independent synthetic source ' + str(i)).encode('utf-8'))
    for i, path in enumerate(artifact_paths):
        target = fake_root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(('independent synthetic product ' + str(i)).encode('utf-8'))

    source_hashes = {p: hashlib.sha256((fake_root / p).read_bytes()).hexdigest() for p in source_paths}
    artifact_hashes = {p: hashlib.sha256((fake_root / p).read_bytes()).hexdigest() for p in artifact_paths}
    host = {'passed': True, 'evidence': 'E1-real-txn-and-mocked-IPC',
            'volume_checks': 5907, 'parser_checks': 4441242, 'parser_cases': 66,
            'owner_checks': 751802, 'owner_cases': 58}

    def case(mode, state, transport):
        success = mode == 'verify'
        return {'mode': mode, 'state': state, 'transport': transport,
                'passed': True, 'phase': 6 if success else 7,
                'reads': 1 if mode == 'verify-readerror' else 259,
                'verified': success, 'normal_discard': success,
                'root_read_error_injected': mode == 'verify-readerror',
                'old_capture_preserved': True, 'old_stop_preserved': True,
                'disk_bytes_preserved': True}

    good = {'passed': True, 'evidence': 'E2-real-isolated-readonly-volume-and-attestation',
            'fixture_scope': 'Synthetic gate input only; not a guest or hosted-run record',
            'source_head': 'b' * 40, 'source_sha256': source_hashes,
            'production_sha256': artifact_hashes, 'guest_boots': 10, 'user_denials': 10,
            'production_artifacts_restored': True, 'base_disk_unchanged': True,
            'candidate_activation': False, 'driver_grants': False,
            'mount_requested': False, 'physical_hardware': False,
            'cases': [case(mode, state, transport)
                      for mode, state in (('verify', 'STABLE'), ('verify', 'PENDING'),
                                          ('verify-readerror', 'STABLE'),
                                          ('verify-badreply', 'STABLE'),
                                          ('verify-dropped', 'STABLE'))
                      for transport in ('legacy', 'modern')]}

    def check(value, accept, host_value=host):
        global checks
        namespace = {'ROOT': fake_root, 'hashlib': hashlib, 'result': {
            'persist_verify_host': host_value, 'persist_verify_qemu': value}}
        try:
            exec(gate_code, namespace)
        except SystemExit:
            assert not accept
        else:
            assert accept
        checks += 1

    check(good, True)
    check(good, False, None)
    for key, valid in host.items():
        bad_host = dict(host); bad_host[key] = False if type(valid) is bool else 'stale'
        check(good, False, bad_host)
        bad_host = dict(host); bad_host.pop(key)
        check(good, False, bad_host)

    bad = copy.deepcopy(good); bad['passed'] = False; check(bad, False)
    bad = copy.deepcopy(good); bad['evidence'] = 'E1-synthetic-only'; check(bad, False)
    bad = copy.deepcopy(good); bad.pop('source_sha256'); check(bad, False)
    for path in source_paths:
        bad = copy.deepcopy(good); bad['source_sha256'][path] = '0' * 64; check(bad, False)
        bad = copy.deepcopy(good); bad['source_sha256'].pop(path); check(bad, False)
    bad = copy.deepcopy(good); bad.pop('production_sha256'); check(bad, False)
    for path in artifact_paths:
        bad = copy.deepcopy(good); bad['production_sha256'][path] = '0' * 64; check(bad, False)
        bad = copy.deepcopy(good); bad['production_sha256'].pop(path); check(bad, False)

    for key, value in (('evidence', 'E1-real-txn-and-mocked-IPC'),
                       ('source_head', 'g' * 40), ('source_head', 'b' * 39),
                       ('guest_boots', 9), ('user_denials', 9)):
        bad = copy.deepcopy(good); bad[key] = value; check(bad, False)

    bad = copy.deepcopy(good); bad['cases'].pop(); check(bad, False)
    bad = copy.deepcopy(good); bad['cases'][1] = copy.deepcopy(bad['cases'][0]); check(bad, False)
    for index, field, value in ((0, 'mode', 'verify-readerror'),
                                (0, 'state', 'PENDING'),
                                (0, 'transport', 'modern'),
                                (2, 'mode', 'verify-readerror'),
                                (0, 'mode', 'unknown')):
        bad = copy.deepcopy(good); bad['cases'][index][field] = value; check(bad, False)

    for index, original in enumerate(good['cases']):
        mutations = (
            ('passed', False), ('passed', 1), ('phase', 7 if original['phase'] == 6 else 6),
            ('phase', True), ('reads', True), ('verified', not original['verified']),
            ('verified', int(original['verified'])), ('normal_discard', not original['normal_discard']),
            ('normal_discard', int(original['normal_discard'])),
            ('root_read_error_injected', not original['root_read_error_injected']),
            ('root_read_error_injected', int(original['root_read_error_injected'])),
            ('old_capture_preserved', False), ('old_capture_preserved', 1),
            ('old_stop_preserved', False), ('old_stop_preserved', 1),
            ('disk_bytes_preserved', False), ('disk_bytes_preserved', 1),
        )
        for field, value in mutations:
            bad = copy.deepcopy(good); bad['cases'][index][field] = value; check(bad, False)
        for reads in (0, 1, 2, 258, 516):
            if reads == original['reads']:
                continue
            bad = copy.deepcopy(good); bad['cases'][index]['reads'] = reads; check(bad, False)
        bad = copy.deepcopy(good); bad['cases'][index].pop('reads'); check(bad, False)
        if original['mode'] != 'verify-readerror':
            bad = copy.deepcopy(good); bad['cases'][index]['root_read_error_injected'] = True; check(bad, False)

    for key in ('production_artifacts_restored', 'base_disk_unchanged'):
        bad = copy.deepcopy(good); bad.pop(key); check(bad, False)
        bad = copy.deepcopy(good); bad[key] = False; check(bad, False)
        bad = copy.deepcopy(good); bad[key] = 1; check(bad, False)
    for key in ('candidate_activation', 'driver_grants', 'mount_requested', 'physical_hardware'):
        bad = copy.deepcopy(good); bad[key] = True; check(bad, False)
        bad = copy.deepcopy(good); bad[key] = 0; check(bad, False)

    for path in source_paths:
        target = fake_root / path
        original = target.read_bytes(); target.write_bytes(original + b' changed')
        check(good, False)
        target.write_bytes(original)
        check(good, True)
    for path in artifact_paths:
        target = fake_root / path
        original = target.read_bytes(); target.write_bytes(original + b' changed')
        check(good, False)
        target.write_bytes(original)
        check(good, True)

print('PERSIST VERIFY EVIDENCE: ' + str(checks) + ' actual release-gate checks')
