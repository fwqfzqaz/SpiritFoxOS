"""Exercise the actual release gate's snapshot evidence bindings."""
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
                   'actual-package-release-snapshot-AST', 'exec')


terminal_paths_node = assignment('terminal_source_paths')
terminal_artifacts_node = assignment('terminal_artifact_paths')
snapshot_paths_node = assignment('snapshot_source_paths')
snapshot_hash_node = assignment('snapshot_source_sha256')
artifact_hash_node = assignment('terminal_artifact_sha256')
guest_node = assignment('snapshot_guest')
cases_node = assignment('snapshot_cases')
release_if = next(n for n in tree.body if isinstance(n, ast.If) and
                  any(isinstance(x, ast.Name) and x.id == 'snapshot_guest' for x in ast.walk(n.test)))
gate_code = module([terminal_paths_node, terminal_artifacts_node, snapshot_paths_node,
                    snapshot_hash_node, artifact_hash_node, guest_node, cases_node, release_if])
snapshot_paths = (ast.literal_eval(terminal_paths_node.value) +
                  ast.literal_eval(snapshot_paths_node.value.right))
artifact_paths = ast.literal_eval(terminal_artifacts_node.value)
checks = 0

with tempfile.TemporaryDirectory(prefix='snapshot-evidence-', dir=ROOT / 'build') as temporary:
    fake_root = Path(temporary)
    for i, path in enumerate(snapshot_paths):
        target = fake_root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text('source ' + str(i), encoding='utf-8')
    for i, path in enumerate(artifact_paths):
        target = fake_root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(('product ' + str(i)).encode())

    sources = {p: hashlib.sha256((fake_root / p).read_bytes()).hexdigest() for p in snapshot_paths}
    products = {p: hashlib.sha256((fake_root / p).read_bytes()).hexdigest() for p in artifact_paths}
    host = {'passed': True, 'evidence': 'E1-host-independent-canonical-vectors',
            'parser_checks': 8197, 'volume_checks': 87, 'owner_checks': 170613, 'trust_checks': 95}

    def case(mode, transport):
        return {'mode': mode, 'transport': transport, 'passed': True,
                'capture_failed_closed': True, 'old_provider_alive': True,
                'old_stop_unchanged': True, 'no_unknown_write': True,
                'disk_bytes_preserved': True}

    good = {'passed': True, 'evidence': 'E2-real-snapshot-reply-quarantine',
            'source_head': 'a' * 40, 'source_sha256': sources, 'production_sha256': products,
            'guest_boots': 4, 'user_denials': 4,
            'production_artifacts_restored': True, 'base_disk_unchanged': True,
            'candidate_activation': False, 'provider_lifecycle': False,
            'mount_requested': False, 'physical_hardware': False,
            'cases': [case(mode, transport)
                      for mode in ('snapshot-badreply', 'snapshot-dropped')
                      for transport in ('legacy', 'modern')]}

    def check(value, accept, host_value=host):
        global checks
        namespace = {'ROOT': fake_root, 'hashlib': hashlib, 'result': {
            'persist_snapshot_host': host_value, 'persist_snapshot_qemu': value}}
        try:
            exec(gate_code, namespace)
        except SystemExit:
            assert not accept
        else:
            assert accept
        checks += 1

    check(good, True)
    bad = copy.deepcopy(good); bad['passed'] = False; check(bad, False)
    bad = copy.deepcopy(good); bad['evidence'] = 'E1-mock-only'; check(bad, False)
    bad = copy.deepcopy(good); bad.pop('source_sha256'); check(bad, False)
    for path in snapshot_paths:
        bad = copy.deepcopy(good); bad['source_sha256'][path] = '0' * 64; check(bad, False)
        bad = copy.deepcopy(good); bad['source_sha256'].pop(path); check(bad, False)
    bad = copy.deepcopy(good); bad['production_sha256'][artifact_paths[0]] = '0' * 64; check(bad, False)
    bad = copy.deepcopy(good); bad['production_sha256'].pop(artifact_paths[0]); check(bad, False)
    bad = copy.deepcopy(good); bad['cases'][0]['mode'] = 'snapshot-dropped'; check(bad, False)
    bad = copy.deepcopy(good); bad['cases'][1]['transport'] = 'legacy'; check(bad, False)
    bad = copy.deepcopy(good); bad['cases'].pop(); check(bad, False)

    required = ('passed', 'capture_failed_closed', 'old_provider_alive',
                'old_stop_unchanged', 'no_unknown_write', 'disk_bytes_preserved')
    for index in range(4):
        for field in required:
            bad = copy.deepcopy(good); bad['cases'][index][field] = False; check(bad, False)
        bad = copy.deepcopy(good); bad['cases'][index]['passed'] = 1; check(bad, False)
    for key in ('production_artifacts_restored', 'base_disk_unchanged'):
        bad = copy.deepcopy(good); bad.pop(key); check(bad, False)
        bad = copy.deepcopy(good); bad[key] = False; check(bad, False)
    for key in ('candidate_activation', 'provider_lifecycle', 'mount_requested', 'physical_hardware'):
        bad = copy.deepcopy(good); bad[key] = True; check(bad, False)
    for key, value in (('source_head', 'z' * 40), ('source_head', 'a' * 39),
                       ('guest_boots', 3), ('user_denials', 3)):
        bad = copy.deepcopy(good); bad[key] = value; check(bad, False)
    check(good, False, host_value=dict(host, trust_checks=94))

    target = fake_root / snapshot_paths[0]
    original = target.read_bytes(); target.write_bytes(original + b' changed')
    check(good, False); target.write_bytes(original)
    target = fake_root / artifact_paths[0]
    original = target.read_bytes(); target.write_bytes(original + b' changed')
    check(good, False); target.write_bytes(original)

print('PERSIST SNAPSHOT EVIDENCE: ' + str(checks) + ' actual release-gate checks')
