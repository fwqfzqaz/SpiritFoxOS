"""Exercise the actual candidate-idle release gate against synthetic records."""
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


def module(nodes):
    return compile(ast.fix_missing_locations(ast.Module(body=nodes, type_ignores=[])),
                   'actual-package-release-idle-AST', 'exec')


terminal_sources_node = assignment('terminal_source_paths')
source_paths = ast.literal_eval(terminal_sources_node.value)
terminal_artifacts_node = assignment('terminal_artifact_paths')
artifact_paths = ast.literal_eval(terminal_artifacts_node.value)
idle_sources_node = assignment('idle_source_paths')
idle_hash_node = assignment('idle_source_sha256')
artifact_hash_node = assignment('terminal_artifact_sha256')
idle_guest_node = assignment('idle_guest')
idle_cases_node = assignment('idle_cases')
idle_validator = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'idle_case_valid')
release_if = next(n for n in tree.body if isinstance(n, ast.If) and
                  any(isinstance(x, ast.Name) and x.id == 'idle_guest' for x in ast.walk(n.test)))
gate_code = module([terminal_sources_node, terminal_artifacts_node, idle_sources_node,
                    idle_hash_node, artifact_hash_node, idle_guest_node, idle_cases_node,
                    idle_validator, release_if])
checks = 0

with tempfile.TemporaryDirectory(prefix='idle-evidence-', dir=ROOT / 'build') as temporary:
    fake_root = Path(temporary)
    path_namespace = {}
    exec(module([terminal_sources_node, idle_sources_node]), path_namespace)
    all_source_paths = path_namespace['idle_source_paths']
    for i, path in enumerate(all_source_paths):
        target = fake_root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text('source ' + str(i), encoding='utf-8')
    for i, path in enumerate(artifact_paths):
        target = fake_root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(('product ' + str(i)).encode())

    source_hashes = {p: hashlib.sha256((fake_root / p).read_bytes()).hexdigest() for p in all_source_paths}
    artifact_hashes = {p: hashlib.sha256((fake_root / p).read_bytes()).hexdigest() for p in artifact_paths}
    host = {'passed': True, 'evidence': 'E1-host-mocked-kernel',
            'parser_checks': 4173, 'owner_checks': 437812}

    def case(mode, transport):
        values = {
            'healthy': (6, True, True, True, True, True, 1),
            'idle-delayed': (6, True, True, True, True, True, 2),
            'idle-badprobe': (7, False, False, False, False, False, 0),
            'idle-dropdiscard': (7, True, True, False, False, False, 0),
        }
        phase, probe, ordinary_denied, discard, death, retired, observations = values[mode]
        return {'mode': mode, 'transport': transport, 'passed': True,
                'disk_bytes_preserved': True, 'old_stop_snapshot_preserved': True,
                'phase': phase, 'probe': probe, 'ordinary_denied': ordinary_denied,
                'discard': discard, 'death': death, 'retired': retired,
                'observations': observations, 'pid': 13}

    good = {'passed': True, 'evidence': 'E2-real-private-candidate-IPC-and-exit',
            'source_head': 'a' * 40, 'source_sha256': source_hashes,
            'production_sha256': artifact_hashes, 'guest_boots': 8, 'user_denials': 8,
            'production_artifacts_restored': True, 'base_disk_unchanged': True,
            'candidate_activation': False, 'driver_grants': False,
            'mount_requested': False, 'physical_hardware': False,
            'cases': [case(mode, transport)
                      for mode in ('healthy', 'idle-badprobe', 'idle-dropdiscard', 'idle-delayed')
                      for transport in ('legacy', 'modern')]}

    def check(value, accept, host_value=host):
        global checks
        namespace = {'ROOT': fake_root, 'hashlib': hashlib,
                     'result': {'persist_idle_host': host_value, 'persist_idle_qemu': value}}
        try:
            exec(gate_code, namespace)
        except SystemExit:
            assert not accept
        else:
            assert accept
        checks += 1

    check(good, True)
    check(good, False, None)
    check(good, False, dict(host, owner_checks=408848))
    check(good, False, dict(host, passed=False))
    bad_host = dict(host); bad_host.pop('owner_checks'); check(good, False, bad_host)

    bad = copy.deepcopy(good); bad['source_sha256'][all_source_paths[0]] = '0' * 64; check(bad, False)
    bad = copy.deepcopy(good); bad['source_sha256'].pop(all_source_paths[-1]); check(bad, False)
    bad = copy.deepcopy(good); bad['production_sha256'][artifact_paths[0]] = '0' * 64; check(bad, False)
    bad = copy.deepcopy(good); bad['production_sha256'].pop(artifact_paths[-1]); check(bad, False)
    for key, value in (('evidence', 'E1-host-mocked-kernel'), ('source_head', 'z' * 40),
                       ('guest_boots', 7), ('user_denials', 7)):
        bad = copy.deepcopy(good); bad[key] = value; check(bad, False)

    bad = copy.deepcopy(good); bad['cases'][1]['mode'] = 'idle-badprobe'; check(bad, False)
    bad = copy.deepcopy(good); bad['cases'][1]['transport'] = 'legacy'; check(bad, False)
    bad = copy.deepcopy(good); bad['cases'][0]['mode'] = 'unknown-mode'; check(bad, False)
    bad = copy.deepcopy(good); bad['cases'].pop(); check(bad, False)

    case_mutations = (
        (0, lambda c: c.__setitem__('passed', False)),
        (0, lambda c: c.__setitem__('disk_bytes_preserved', False)),
        (0, lambda c: c.__setitem__('old_stop_snapshot_preserved', False)),
        (0, lambda c: c.__setitem__('old_stop_snapshot_preserved', 1)),
        (0, lambda c: c.__setitem__('phase', 7)),
        (0, lambda c: c.__setitem__('phase', True)),
        (0, lambda c: c.__setitem__('observations', True)),
        (0, lambda c: c.__setitem__('observations', 0)),
        (0, lambda c: c.__setitem__('pid', True)),
        (0, lambda c: c.__setitem__('pid', 1)),
        (0, lambda c: c.__setitem__('pid', 2147483648)),
        (0, lambda c: c.__setitem__('pid', 13.0)),
        (0, lambda c: c.pop('pid')),
        (0, lambda c: c.__setitem__('probe', 1)),
        (0, lambda c: c.__setitem__('ordinary_denied', 1)),
        (0, lambda c: c.__setitem__('discard', 1)),
        (0, lambda c: c.__setitem__('death', 1)),
        (0, lambda c: c.__setitem__('retired', 1)),
        (2, lambda c: c.__setitem__('probe', True)),       # bad probe must not claim acceptance
        (4, lambda c: c.__setitem__('discard', True)),     # dropped discard must not claim acceptance
        (2, lambda c: c.__setitem__('death', True)),
        (2, lambda c: c.__setitem__('retired', True)),
        (4, lambda c: c.__setitem__('ordinary_denied', False)),
    )
    for index, mutate in case_mutations:
        bad = copy.deepcopy(good); mutate(bad['cases'][index]); check(bad, False)
    bad = copy.deepcopy(good); bad['cases'][7]['observations'] = 1; check(bad, False)

    for key, value in (('production_artifacts_restored', False), ('base_disk_unchanged', False),
                       ('candidate_activation', True), ('driver_grants', True),
                       ('mount_requested', True), ('physical_hardware', True)):
        bad = copy.deepcopy(good); bad[key] = value; check(bad, False)

    target = fake_root / all_source_paths[0]
    original = target.read_bytes(); target.write_bytes(original + b' changed'); check(good, False); target.write_bytes(original)
    target = fake_root / artifact_paths[0]
    original = target.read_bytes(); target.write_bytes(original + b' changed'); check(good, False); target.write_bytes(original)

assert checks == 49
print('PERSIST IDLE EVIDENCE PASS: ' + str(checks) + ' actual release-gate checks')
