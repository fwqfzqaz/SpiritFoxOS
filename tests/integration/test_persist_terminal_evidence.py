"""Reject stale terminal evidence using the actual package release gate."""
import ast
import copy
import hashlib
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parents[2]
gate = ROOT / 'tools/package_release.py'
tree = ast.parse(gate.read_text(encoding='utf-8'))


def assignment(name):
    return next(n for n in tree.body if isinstance(n, ast.Assign) and len(n.targets) == 1 and
                isinstance(n.targets[0], ast.Name) and n.targets[0].id == name)


def module(nodes):
    return compile(ast.fix_missing_locations(ast.Module(body=nodes, type_ignores=[])),
                   'actual-package-release-terminal-AST', 'exec')


source_paths_node = assignment('terminal_source_paths')
artifact_paths_node = assignment('terminal_artifact_paths')
source_paths = ast.literal_eval(source_paths_node.value)
artifact_paths = ast.literal_eval(artifact_paths_node.value)
source_hash_node = assignment('terminal_source_sha256')
artifact_hash_node = assignment('terminal_artifact_sha256')
guest_node = assignment('terminal_guest')
cases_node = assignment('terminal_cases')
case_validator = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'terminal_case_valid')
release_if = next(n for n in tree.body if isinstance(n, ast.If) and
                  any(isinstance(x, ast.Name) and x.id == 'terminal_guest' for x in ast.walk(n.test)))
gate_code = module([source_paths_node, source_hash_node, artifact_paths_node, artifact_hash_node,
                    guest_node, cases_node, case_validator, release_if])
checks = 0

with tempfile.TemporaryDirectory(prefix='terminal-evidence-', dir=ROOT / 'build') as temporary:
    fake_root = Path(temporary)
    for i, path in enumerate(source_paths):
        target = fake_root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text('source ' + str(i), encoding='utf-8')
    for i, path in enumerate(artifact_paths):
        target = fake_root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(('product ' + str(i)).encode())

    sources = {p: hashlib.sha256((fake_root / p).read_bytes()).hexdigest() for p in source_paths}
    products = {p: hashlib.sha256((fake_root / p).read_bytes()).hexdigest() for p in artifact_paths}
    host = {'passed': True, 'evidence': 'E1-host-only', 'server_checks': 550,
            'owner_checks': 642533, 'owner_cases': 187}
    candidate_host = {'passed': True, 'evidence': 'E1-host-real-signatures',
                      'owner_checks': 216690, 'owner_cases': 67,
                      'image_checks': 688, 'signed_fixtures': 8}

    def case(mode, transport):
        values = {'healthy': (3, True, True, True, 1),
                  'delayed': (3, True, True, True, 2),
                  'unknown': (4, False, True, False, 1),
                  'busy': (5, True, False, False, 0)}[mode]
        phase, ack, death, retired, observations = values
        return {'mode': mode, 'transport': transport, 'passed': True,
                'disk_bytes_preserved': True, 'busy_is_injected_cache_result': mode == 'busy',
                'status': {'pid': 21, 'phase': phase, 'observations': observations, 'candidate_inspected': mode in ('healthy','delayed'),
                           'ack_authenticated': ack, 'death_confirmed': death, 'retired': retired}}

    good = {'passed': True, 'evidence': 'E2-real-kernel-IPC-and-provider-exit',
            'source_head': 'a' * 40, 'source_sha256': sources, 'production_sha256': products,
            'guest_boots': 8, 'user_denials': 8,
            'production_artifacts_restored': True, 'base_disk_unchanged': True,
            'candidate_activation': False, 'generic_restart': False, 'physical_hardware': False,
            'cases': [case(mode, transport) for mode in ('healthy', 'unknown', 'delayed', 'busy')
                      for transport in ('legacy', 'modern')]}

    def check(value, accept, candidate=candidate_host):
        global checks
        namespace = {'ROOT': fake_root, 'hashlib': hashlib, 'result': {
            'persist_terminal_host': host, 'persist_terminal_qemu': value,
            'persist_candidate_host': candidate}}
        try:
            exec(gate_code, namespace)
        except SystemExit:
            assert not accept
        else:
            assert accept
        checks += 1

    check(good, True)

    bad = copy.deepcopy(good); bad['source_sha256'][source_paths[0]] = '0' * 64; check(bad, False)
    bad = copy.deepcopy(good); bad['production_sha256'][artifact_paths[0]] = '0' * 64; check(bad, False)
    bad = copy.deepcopy(good); bad['cases'][0]['mode'] = 'delayed'; check(bad, False)
    bad = copy.deepcopy(good); bad['cases'][1]['transport'] = 'legacy'; check(bad, False)
    mutations = (
        lambda c: c[2]['status'].__setitem__('ack_authenticated', True),
        lambda c: c[0]['status'].__setitem__('death_confirmed', False),
        lambda c: c[2]['status'].__setitem__('retired', True),
        lambda c: c[0]['status'].__setitem__('phase', 4),
        lambda c: c[4]['status'].__setitem__('observations', 1),
        lambda c: c[6].__setitem__('busy_is_injected_cache_result', False),
        lambda c: c[0]['status'].__setitem__('candidate_inspected', False),
        lambda c: c[2]['status'].__setitem__('candidate_inspected', True),
        lambda c: c[0]['status'].__setitem__('candidate_inspected', 1),
        lambda c: c[0]['status'].pop('candidate_inspected'),
    )
    for mutate in mutations:
        bad = copy.deepcopy(good); mutate(bad['cases']); check(bad, False)
    for key, value in (('production_artifacts_restored', None), ('generic_restart', True),
                       ('source_head', 'z' * 40), ('user_denials', 7)):
        bad = copy.deepcopy(good)
        if value is None:
            del bad[key]
        else:
            bad[key] = value
        check(bad, False)
    for field in ('observations', 'pid'):
        bad = copy.deepcopy(good); bad['cases'][0]['status'][field] = True; check(bad, False)
    bad_host = dict(host, owner_checks=642532)
    namespace = {'ROOT': fake_root, 'hashlib': hashlib, 'result': {
        'persist_terminal_host': bad_host, 'persist_terminal_qemu': good,
        'persist_candidate_host': candidate_host}}
    try:
        exec(gate_code, namespace)
    except SystemExit:
        checks += 1
    else:
        raise AssertionError('Bad host evidence was accepted')

    check(good, False, candidate=None)
    check(good, False, candidate=dict(candidate_host, image_checks=687))
    check(good, False, candidate=dict(candidate_host, signed_fixtures=7))

    target = fake_root / source_paths[0]
    original = target.read_bytes(); target.write_bytes(original + b' changed'); check(good, False); target.write_bytes(original)
    target = fake_root / artifact_paths[0]
    original = target.read_bytes(); target.write_bytes(original + b' changed'); check(good, False); target.write_bytes(original)

assert checks == 27
print('PERSIST TERMINAL EVIDENCE PASS: ' + str(checks) + ' actual release-gate checks')
