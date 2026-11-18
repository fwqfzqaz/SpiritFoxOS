"""Exercise the actual provisional-cap stage release gate with synthetic records."""
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


artifacts_node = assignment('terminal_artifact_paths')
artifact_hash_node = assignment('terminal_artifact_sha256')
terminal_sources_node = assignment('terminal_source_paths')
source_paths_node = assignment('stage_source_paths')
source_hash_node = assignment('stage_source_sha256')
guest_node = assignment('stage_guest')
host_node = assignment('stage_host')
cases_node = assignment('stage_cases')
validator = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'stage_case_valid')
release_if = next(n for n in tree.body if isinstance(n, ast.If) and
                  any(isinstance(x, ast.Name) and x.id == 'stage_guest' for x in ast.walk(n.test)))
gate_code = compile_nodes([artifacts_node, terminal_sources_node, artifact_hash_node, source_paths_node, source_hash_node,
                           guest_node, host_node, cases_node, validator, release_if],
                          'actual-package-release-persist-stage-AST')
path_context = {}
exec(compile_nodes([artifacts_node, terminal_sources_node, source_paths_node],
                   'actual-stage-source-paths-AST'), path_context)
source_paths = path_context['stage_source_paths']
artifact_paths = path_context['terminal_artifact_paths']
checks = 0

with tempfile.TemporaryDirectory(prefix='persist-stage-evidence-', dir=ROOT / 'build') as temporary:
    fake_root = Path(temporary)
    for i, path in enumerate(source_paths):
        target = fake_root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(('independent synthetic stage source ' + str(i)).encode('utf-8'))
    for i, path in enumerate(artifact_paths):
        target = fake_root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(('independent synthetic production product ' + str(i)).encode('utf-8'))

    source_hashes = {p: hashlib.sha256((fake_root / p).read_bytes()).hexdigest() for p in source_paths}
    product_hashes = {p: hashlib.sha256((fake_root / p).read_bytes()).hexdigest() for p in artifact_paths}
    host = {'passed': True, 'evidence': 'E1-real-stage-codec-and-mocked-owner-IPC',
            'provider_checks': 22098, 'provider_cases': 4003,
            'owner_checks': 1220783, 'owner_cases': 50}
    combinations = [('healthy', state, transport) for state in ('STABLE', 'PENDING')
                    for transport in ('legacy', 'modern')]
    combinations += [(mode, 'STABLE', transport)
                     for mode in ('bad-first-ack', 'lost-second-ack')
                     for transport in ('legacy', 'modern')]

    def case(mode, state, transport):
        success = mode == 'healthy'
        return {'mode': mode, 'state': state, 'transport': transport, 'passed': True,
                'phase': 6 if success else 7,
                'outcome': ('normal-discard-all-four-root-handles-retired' if success
                            else 'quarantined-branches-retired'),
                'alice_denied': True, 'duplicate_denied': True,
                'driver_originals_usable': True, 'root_cap_budget_preserved': True,
                'old_capture_stop_trust_preserved': True, 'disk_bytes_preserved': True}

    good = {'passed': True, 'evidence': 'E2-real-provisional-driver-cap-ownership-and-retirement',
            'source_head': 'b' * 40, 'source_sha256': source_hashes,
            'production_sha256': product_hashes, 'guest_boots': 8,
            'production_artifacts_restored': True, 'base_disk_unchanged': True,
            'ordinary_backend_bound': False, 'candidate_activation': False,
            'physical_hardware': False,
            'cases': [case(*combo) for combo in combinations]}

    def check(value, accept, host_value=host):
        global checks
        namespace = {'ROOT': fake_root, 'hashlib': hashlib,
                     'result': {'persist_stage_host': host_value, 'persist_stage_qemu': value}}
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
        changed = dict(host); changed.pop(key); check(good, False, changed)
    changed = dict(host); changed['passed'] = 1; check(good, False, changed)
    for key in ('provider_checks', 'provider_cases', 'owner_checks', 'owner_cases'):
        for value in (True, float(host[key]), str(host[key])):
            changed = dict(host); changed[key] = value; check(good, False, changed)

    for key, value in (('passed', False), ('passed', 1), ('evidence', 'synthetic'),
                       ('source_head', 'B' * 40), ('source_head', 'b' * 39),
                       ('source_head', 'g' * 40), ('source_head', None),
                       ('guest_boots', 7), ('guest_boots', True), ('guest_boots', 8.0)):
        bad = copy.deepcopy(good); bad[key] = value; check(bad, False)
    for key in ('passed', 'evidence', 'source_head', 'source_sha256', 'production_sha256',
                'guest_boots', 'cases'):
        bad = copy.deepcopy(good); bad.pop(key); check(bad, False)
    bad = copy.deepcopy(good); bad['cases'] = None; check(bad, False)

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
    for key in ('ordinary_backend_bound', 'candidate_activation', 'physical_hardware'):
        for value in (True, 1):
            bad = copy.deepcopy(good); bad[key] = value; check(bad, False)
        bad = copy.deepcopy(good); bad.pop(key); check(bad, False)

    bad = copy.deepcopy(good); bad['cases'].pop(); check(bad, False)
    bad = copy.deepcopy(good); bad['cases'].append(copy.deepcopy(bad['cases'][0])); check(bad, False)
    bad = copy.deepcopy(good); bad['cases'][1] = copy.deepcopy(bad['cases'][0]); check(bad, False)
    bad = copy.deepcopy(good); bad['cases'][0]['transport'] = 'modern'; check(bad, False)
    for index, field, value in ((0, 'mode', 'bad-first-ack'), (0, 'state', 'PENDING'),
                                (0, 'transport', 'modern'), (4, 'mode', 'lost-second-ack'),
                                (4, 'state', 'PENDING'), (4, 'transport', 'modern'),
                                (6, 'mode', 'bad-first-ack'), (6, 'state', 'PENDING')):
        bad = copy.deepcopy(good); bad['cases'][index][field] = value; check(bad, False)

    fields = ('alice_denied', 'duplicate_denied', 'driver_originals_usable',
              'root_cap_budget_preserved', 'old_capture_stop_trust_preserved', 'disk_bytes_preserved')
    for index, original in enumerate(good['cases']):
        for field, value in (('passed', False), ('passed', 1), ('phase', True),
                             ('phase', 6.0), ('phase', 7 if original['phase'] == 6 else 6),
                             ('outcome', 'wrong')):
            bad = copy.deepcopy(good); bad['cases'][index][field] = value; check(bad, False)
        for field in ('mode', 'state', 'transport', 'passed', 'phase', 'outcome'):
            bad = copy.deepcopy(good); bad['cases'][index].pop(field); check(bad, False)
        for field in fields:
            for value in (False, 1):
                bad = copy.deepcopy(good); bad['cases'][index][field] = value; check(bad, False)
            bad = copy.deepcopy(good); bad['cases'][index].pop(field); check(bad, False)

    for path in source_paths:
        target = fake_root / path
        original = target.read_bytes(); target.write_bytes(original + b' changed')
        check(good, False)
        target.write_bytes(original); check(good, True)
    for path in artifact_paths:
        target = fake_root / path
        original = target.read_bytes(); target.write_bytes(original + b' changed')
        check(good, False)
        target.write_bytes(original); check(good, True)

print('PERSIST STAGE EVIDENCE: ' + str(checks) + ' actual release-gate checks (synthetic only)')
