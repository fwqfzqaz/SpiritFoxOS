"""Exercise the actual hold release gate with synthetic evidence only."""
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


def compile_nodes(nodes, filename):
    return compile(ast.fix_missing_locations(ast.Module(body=nodes, type_ignores=[])), filename, 'exec')


terminal_paths = assignment('terminal_source_paths')
artifact_paths_node = assignment('terminal_artifact_paths')
artifact_hash = assignment('terminal_artifact_sha256')
hold_paths = assignment('hold_source_paths')
hold_hash = assignment('hold_source_sha256')
guest_node = assignment('hold_guest')
cases_node = assignment('hold_cases')
validator = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'hold_case_valid')
release_if = next(n for n in tree.body if isinstance(n, ast.If) and
                  any(isinstance(x, ast.Name) and x.id == 'hold_guest' for x in ast.walk(n.test)))
gate_code = compile_nodes([terminal_paths, artifact_paths_node, artifact_hash,
                           hold_paths, hold_hash, guest_node, cases_node,
                           validator, release_if], 'actual-package-release-hold-AST')
paths_context = {}
exec(compile_nodes([terminal_paths, artifact_paths_node, hold_paths],
                   'actual-hold-source-path-AST'), paths_context)
source_paths = paths_context['hold_source_paths']
artifact_paths = paths_context['terminal_artifact_paths']
checks = 0

with tempfile.TemporaryDirectory(prefix='hold-evidence-', dir=ROOT / 'build') as temporary:
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
    product_hashes = {p: hashlib.sha256((fake_root / p).read_bytes()).hexdigest() for p in artifact_paths}
    host = {'passed': True, 'evidence': 'E1-held-owner-mocked-kernel-transport',
            'checks': 247421, 'cases': 14}

    def case(mode, state, transport, pid):
        success = mode == 'healthy'
        return {'mode': mode, 'state': state, 'transport': transport, 'passed': True,
                'held_pid': pid, 'verified_reads': 259, 'phase': 6 if success else 7,
                'discard_outcome': 'discarded' if success else 'discard-ack-dropped',
                'old_hold_deadline_expired': True, 'alice_denied_hold_and_discard': True,
                'ram_access_retained': True, 'persist_read_blocked': True,
                'disk_bytes_preserved': True, 'old_capture_stop_preserved': True}

    combinations = [('healthy', state, transport) for state in ('STABLE', 'PENDING')
                    for transport in ('legacy', 'modern')]
    combinations += [('idle-dropdiscard', 'STABLE', transport) for transport in ('legacy', 'modern')]
    good = {'passed': True, 'evidence': 'E2-real-held-candidate-and-delayed-explicit-discard',
            'fixture_scope': 'Synthetic gate input only; not a guest or hosted-run record',
            'source_head': 'b' * 40, 'source_sha256': source_hashes,
            'production_sha256': product_hashes, 'guest_boots': 6, 'user_denials': 12,
            'production_artifacts_restored': True, 'base_disk_unchanged': True,
            'candidate_activation': False, 'driver_grants': False,
            'mount_requested': False, 'physical_hardware': False,
            'cases': [case(mode, state, transport, 31 + i)
                      for i, (mode, state, transport) in enumerate(combinations)]}

    def check(value, accept, host_value=host):
        global checks
        namespace = {'ROOT': fake_root, 'hashlib': hashlib,
                     'result': {'persist_hold_host': host_value, 'persist_hold_qemu': value}}
        try:
            exec(gate_code, namespace)
        except SystemExit:
            assert not accept
        else:
            assert accept
        checks += 1

    check(good, True)
    check(good, False, None)
    for key, expected in host.items():
        changed = dict(host); changed[key] = not expected if type(expected) is bool else 'stale'
        check(good, False, changed)
        changed = dict(host); changed.pop(key)
        check(good, False, changed)
    for key in ('passed',):
        changed = dict(host); changed[key] = 1; check(good, False, changed)
    for key in ('checks', 'cases'):
        for value in (True, float(host[key]), str(host[key])):
            changed = dict(host); changed[key] = value; check(good, False, changed)

    for key, value in (('passed', False), ('passed', 1), ('evidence', 'synthetic-wrong'),
                       ('source_head', 'B' * 40), ('source_head', 'b' * 39),
                       ('guest_boots', 5), ('user_denials', 11)):
        bad = copy.deepcopy(good); bad[key] = value; check(bad, False)
    for key, value in (('guest_boots', True), ('guest_boots', 6.0),
                       ('user_denials', True), ('user_denials', 12.0)):
        bad = copy.deepcopy(good); bad[key] = value; check(bad, False)
    for key in ('passed', 'evidence', 'source_head', 'guest_boots', 'user_denials',
                'source_sha256', 'production_sha256', 'cases'):
        bad = copy.deepcopy(good); bad.pop(key); check(bad, False)

    bad = copy.deepcopy(good); bad['source_sha256'] = {}; check(bad, False)
    for path in source_paths:
        bad = copy.deepcopy(good); bad['source_sha256'][path] = '0' * 64; check(bad, False)
        bad = copy.deepcopy(good); bad['source_sha256'].pop(path); check(bad, False)
    bad = copy.deepcopy(good); bad['production_sha256'] = {}; check(bad, False)
    for path in artifact_paths:
        bad = copy.deepcopy(good); bad['production_sha256'][path] = '0' * 64; check(bad, False)
        bad = copy.deepcopy(good); bad['production_sha256'].pop(path); check(bad, False)

    for key in ('production_artifacts_restored', 'base_disk_unchanged'):
        for value in (False, 1):
            bad = copy.deepcopy(good); bad[key] = value; check(bad, False)
        bad = copy.deepcopy(good); bad.pop(key); check(bad, False)
    for key in ('candidate_activation', 'driver_grants', 'mount_requested', 'physical_hardware'):
        for value in (True, 0):
            bad = copy.deepcopy(good); bad[key] = value; check(bad, False)
        bad = copy.deepcopy(good); bad.pop(key); check(bad, False)

    bad = copy.deepcopy(good); bad['cases'].pop(); check(bad, False)
    bad = copy.deepcopy(good); bad['cases'].append(copy.deepcopy(bad['cases'][0])); check(bad, False)
    bad = copy.deepcopy(good); bad['cases'][1] = copy.deepcopy(bad['cases'][0]); check(bad, False)
    for index, key, value in ((0, 'state', 'PENDING'), (0, 'transport', 'modern'),
                              (0, 'mode', 'idle-dropdiscard'),
                              (4, 'mode', 'healthy'), (4, 'state', 'PENDING'),
                              (4, 'transport', 'modern')):
        bad = copy.deepcopy(good); bad['cases'][index][key] = value; check(bad, False)

    for index, original in enumerate(good['cases']):
        success = original['mode'] == 'healthy'
        for field, value in (('passed', False), ('passed', 1),
                             ('held_pid', 1), ('held_pid', 2147483648),
                             ('held_pid', True), ('held_pid', '31'),
                             ('phase', 7 if success else 6), ('phase', True),
                             ('phase', False), ('discard_outcome', 'wrong')):
            bad = copy.deepcopy(good); bad['cases'][index][field] = value; check(bad, False)
        for reads in (True, False, 258, 516):
            bad = copy.deepcopy(good); bad['cases'][index]['verified_reads'] = reads; check(bad, False)
        for field in ('held_pid', 'verified_reads', 'phase', 'discard_outcome'):
            bad = copy.deepcopy(good); bad['cases'][index].pop(field); check(bad, False)
        for field in ('old_hold_deadline_expired', 'alice_denied_hold_and_discard',
                      'ram_access_retained', 'persist_read_blocked', 'disk_bytes_preserved',
                      'old_capture_stop_preserved'):
            for value in (False, 1):
                bad = copy.deepcopy(good); bad['cases'][index][field] = value; check(bad, False)
            bad = copy.deepcopy(good); bad['cases'][index].pop(field); check(bad, False)
        bad = copy.deepcopy(good); bad['cases'][index]['verified_reads'] = 515; check(bad, True)

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

print('PERSIST HOLD EVIDENCE: ' + str(checks) + ' actual release-gate checks (synthetic only)')
