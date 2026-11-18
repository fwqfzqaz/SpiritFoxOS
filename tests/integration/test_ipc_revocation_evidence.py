"""Exercise the actual IPC revocation release gate with synthetic records."""
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


terminal_artifacts = assignment('terminal_artifact_paths')
terminal_hashes = assignment('terminal_artifact_sha256')
revoke_paths = assignment('revoke_source_paths')
revoke_hashes = assignment('revoke_source_sha256')
guest_node = assignment('revoke_guest')
host_node = assignment('revoke_host')
cases_node = assignment('revoke_cases')
validator = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'revoke_case_valid')
release_if = next(n for n in tree.body if isinstance(n, ast.If) and
                  any(isinstance(x, ast.Name) and x.id == 'revoke_guest' for x in ast.walk(n.test)))
gate_code = compile_nodes([terminal_artifacts, terminal_hashes, revoke_paths, revoke_hashes,
                           guest_node, host_node, cases_node, validator, release_if],
                          'actual-package-release-ipc-revocation-AST')
paths_context = {}
exec(compile_nodes([terminal_artifacts, revoke_paths], 'actual-revoke-paths-AST'), paths_context)
source_paths = paths_context['revoke_source_paths']
artifact_paths = paths_context['terminal_artifact_paths']
checks = 0

with tempfile.TemporaryDirectory(prefix='ipc-revoke-evidence-', dir=ROOT / 'build') as temporary:
    fake_root = Path(temporary)
    for i, path in enumerate(source_paths):
        target = fake_root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(('independent synthetic revoke source ' + str(i)).encode('utf-8'))
    for i, path in enumerate(artifact_paths):
        target = fake_root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(('independent synthetic production product ' + str(i)).encode('utf-8'))

    source_hashes = {p: hashlib.sha256((fake_root / p).read_bytes()).hexdigest() for p in source_paths}
    product_hashes = {p: hashlib.sha256((fake_root / p).read_bytes()).hexdigest() for p in artifact_paths}
    host = {'passed': True, 'evidence': 'E1-real-kernel-mocked-user-copy', 'checks': 9151}
    combos = [(transport, cpus) for transport in ('legacy', 'modern') for cpus in (1, 2)]

    def case(transport, cpus):
        return {'transport': transport, 'cpus': cpus, 'passed': True, 'app_checks': 204,
                'alice_denied': True, 'root_caps_preserved': True,
                'ordinary_service_retained': True, 'disk_bytes_preserved': True}

    good = {'passed': True, 'evidence': 'E2-real-queued-and-closed-parent-cap-revocation',
            'fixture_scope': 'Synthetic gate input only; not a guest or hosted-run record',
            'source_head': 'b' * 40, 'source_sha256': source_hashes,
            'production_sha256': product_hashes, 'guest_boots': 4,
            'production_artifacts_preserved': True, 'base_disk_unchanged': True,
            'driver_authority_handoff': False, 'physical_hardware': False,
            'cases': [case(transport, cpus) for transport, cpus in combos]}

    def check(value, accept, host_value=host):
        global checks
        namespace = {'ROOT': fake_root, 'hashlib': hashlib,
                     'result': {'ipc_revocation_host': host_value,
                                'ipc_revocation_qemu': value}}
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
        bad_host = dict(host); bad_host[key] = not expected if type(expected) is bool else 'stale'
        check(good, False, bad_host)
        bad_host = dict(host); bad_host.pop(key)
        check(good, False, bad_host)
    bad_host = dict(host); bad_host['passed'] = 1; check(good, False, bad_host)
    for value in (True, 9151.0, '9151'):
        bad_host = dict(host); bad_host['checks'] = value; check(good, False, bad_host)

    for key, value in (('passed', False), ('passed', 1), ('evidence', 'E1-synthetic'),
                       ('source_head', 'B' * 40), ('source_head', 'b' * 39),
                       ('source_head', 'z' * 40), ('guest_boots', 3),
                       ('guest_boots', True), ('guest_boots', 4.0)):
        bad = copy.deepcopy(good); bad[key] = value; check(bad, False)
    for key in ('passed', 'evidence', 'source_head', 'source_sha256', 'production_sha256',
                'guest_boots', 'cases'):
        bad = copy.deepcopy(good); bad.pop(key); check(bad, False)

    bad = copy.deepcopy(good); bad['source_sha256'] = {}; check(bad, False)
    for path in source_paths:
        bad = copy.deepcopy(good); bad['source_sha256'][path] = '0' * 64; check(bad, False)
        bad = copy.deepcopy(good); bad['source_sha256'].pop(path); check(bad, False)
    bad = copy.deepcopy(good); bad['production_sha256'] = {}; check(bad, False)
    for path in artifact_paths:
        bad = copy.deepcopy(good); bad['production_sha256'][path] = '0' * 64; check(bad, False)
        bad = copy.deepcopy(good); bad['production_sha256'].pop(path); check(bad, False)

    for key in ('production_artifacts_preserved', 'base_disk_unchanged'):
        for value in (False, 1):
            bad = copy.deepcopy(good); bad[key] = value; check(bad, False)
        bad = copy.deepcopy(good); bad.pop(key); check(bad, False)
    for key in ('driver_authority_handoff', 'physical_hardware'):
        for value in (True, 0):
            bad = copy.deepcopy(good); bad[key] = value; check(bad, False)
        bad = copy.deepcopy(good); bad.pop(key); check(bad, False)

    bad = copy.deepcopy(good); bad['cases'].pop(); check(bad, False)
    bad = copy.deepcopy(good); bad['cases'].append(copy.deepcopy(bad['cases'][0])); check(bad, False)
    bad = copy.deepcopy(good); bad['cases'][1] = copy.deepcopy(bad['cases'][0]); check(bad, False)
    for index, field, value in ((0, 'transport', 'unknown'), (0, 'cpus', 2),
                                (0, 'cpus', True), (2, 'transport', 'legacy'),
                                (3, 'cpus', 1)):
        bad = copy.deepcopy(good); bad['cases'][index][field] = value; check(bad, False)

    for index, original in enumerate(good['cases']):
        for field, value in (('passed', False), ('passed', 1), ('cpus', 0), ('cpus', 3),
                             ('cpus', True), ('cpus', 1.0), ('cpus', '1'),
                             ('app_checks', 203), ('app_checks', True),
                             ('app_checks', 204.0), ('app_checks', '204')):
            bad = copy.deepcopy(good); bad['cases'][index][field] = value; check(bad, False)
        for field in ('transport', 'cpus', 'passed', 'app_checks'):
            bad = copy.deepcopy(good); bad['cases'][index].pop(field); check(bad, False)
        for field in ('alice_denied', 'root_caps_preserved',
                      'ordinary_service_retained', 'disk_bytes_preserved'):
            for value in (False, 1):
                bad = copy.deepcopy(good); bad['cases'][index][field] = value; check(bad, False)
            bad = copy.deepcopy(good); bad['cases'][index].pop(field); check(bad, False)

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

print('IPC REVOCATION EVIDENCE: ' + str(checks) + ' actual release-gate checks (synthetic only)')
