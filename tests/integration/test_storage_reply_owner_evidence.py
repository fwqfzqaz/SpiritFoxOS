"""Exercise the actual storage reply-owner release gate using synthetic evidence."""
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


terminal_sources_node = assignment('terminal_source_paths')
terminal_products_node = assignment('terminal_artifact_paths')
terminal_products_hash_node = assignment('terminal_artifact_sha256')
sources_node = assignment('reply_owner_source_paths')
source_hash_node = assignment('reply_owner_source_sha256')
host_node = assignment('reply_owner_host')
guest_node = assignment('reply_owner_guest')
cases_node = assignment('reply_owner_cases')
validator = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and
                 n.name == 'reply_owner_case_valid')
release_if = next(n for n in tree.body if isinstance(n, ast.If) and
                  any(isinstance(x, ast.Name) and x.id == 'reply_owner_guest'
                      for x in ast.walk(n.test)))
gate_code = compile_nodes([terminal_sources_node, terminal_products_node,
                           terminal_products_hash_node, sources_node, source_hash_node,
                           host_node, guest_node, cases_node, validator, release_if],
                          'actual-package-release-storage-reply-owner-AST')
path_context = {}
exec(compile_nodes([terminal_sources_node, terminal_products_node, sources_node],
                   'actual-storage-reply-owner-paths-AST'), path_context)
source_paths = path_context['reply_owner_source_paths']
product_paths = path_context['terminal_artifact_paths']
checks = 0

with tempfile.TemporaryDirectory(prefix='storage-reply-owner-evidence-', dir=ROOT / 'build') as temporary:
    fake_root = Path(temporary)
    for i, path in enumerate(source_paths):
        target = fake_root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(('independent synthetic reply-owner source ' + str(i)).encode('utf-8'))
    for i, path in enumerate(product_paths):
        target = fake_root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(('independent synthetic production product ' + str(i)).encode('utf-8'))

    source_hashes = {p: hashlib.sha256((fake_root / p).read_bytes()).hexdigest() for p in source_paths}
    product_hashes = {p: hashlib.sha256((fake_root / p).read_bytes()).hexdigest() for p in product_paths}
    host = {'passed': True, 'evidence': 'E1-pure-state-and-mocked-Root-integration',
            'primitive_checks': 24243, 'primitive_cases': 1003,
            'controller_checks': 546258, 'controller_cases': 19}
    combinations = [('known-release', state, transport)
                    for state in ('STABLE', 'PENDING') for transport in ('legacy', 'modern')]
    combinations += [('explicit-quarantine', 'STABLE', transport)
                     for transport in ('legacy', 'modern')]

    def case(mode, state, transport):
        known = mode == 'known-release'
        return {'mode': mode, 'state': state, 'transport': transport,
                'passed': True, 'lease_serials': [1, 2] if known else [1],
                'alice_denied': True, 'root_transport_gated': True, 'driver_alive': True,
                'ram_echo_retained': True, 'disk_bytes_preserved': True,
                'old_stop_capture_trust_preserved': True,
                'known_root_reads_resumed': known, 'normal_discard': known,
                'explicit_quarantine': not known, 'candidate_branches_retired': not known}

    good = {'passed': True, 'evidence': 'E2-real-Root-reply-gate-and-explicit-quarantine',
            'source_head': 'c' * 40, 'source_sha256': source_hashes,
            'production_sha256': product_hashes, 'guest_boots': 6,
            'production_artifacts_preserved': True, 'base_disk_unchanged': True,
            'candidate_driver_rpc': False, 'real_driver_timeout': False,
            'ordinary_backend_bound': False, 'candidate_activation': False,
            'physical_hardware': False,
            'cases': [case(*combo) for combo in combinations]}

    def check(value, accept, host_value=host):
        global checks
        namespace = {'ROOT': fake_root, 'hashlib': hashlib,
                     'result': {'storage_reply_owner_host': host_value,
                                'storage_reply_owner_qemu': value}}
        try:
            exec(gate_code, namespace)
        except SystemExit:
            assert not accept
        else:
            assert accept
        checks += 1

    check(good, True)
    check(good, False, None)
    check(good, False, {})
    for key in host:
        changed = dict(host); changed.pop(key); check(good, False, changed)
        changed = dict(host); changed[key] = False if type(host[key]) is bool else 'stale'
        check(good, False, changed)
    for key in ('primitive_checks', 'primitive_cases', 'controller_checks', 'controller_cases'):
        for value in (True, float(host[key]), str(host[key])):
            changed = dict(host); changed[key] = value; check(good, False, changed)

    for key, value in (('passed', False), ('passed', 1), ('evidence', 'synthetic'),
                       ('source_head', 'C' * 40), ('source_head', 'c' * 39),
                       ('source_head', 'z' * 40), ('source_head', None),
                       ('guest_boots', 5), ('guest_boots', True), ('guest_boots', 6.0)):
        bad = copy.deepcopy(good); bad[key] = value; check(bad, False)
    for key in ('passed', 'evidence', 'source_head', 'source_sha256', 'production_sha256',
                'guest_boots', 'cases'):
        bad = copy.deepcopy(good); bad.pop(key); check(bad, False)
    bad = copy.deepcopy(good); bad['cases'] = None; check(bad, False)

    for mapping_name, paths in (('source_sha256', source_paths),
                                ('production_sha256', product_paths)):
        bad = copy.deepcopy(good); bad[mapping_name] = {}; check(bad, False)
        for path in paths:
            bad = copy.deepcopy(good); bad[mapping_name][path] = '0' * 64; check(bad, False)
            bad = copy.deepcopy(good); bad[mapping_name].pop(path); check(bad, False)

    for key in ('production_artifacts_preserved', 'base_disk_unchanged'):
        for value in (False, 1):
            bad = copy.deepcopy(good); bad[key] = value; check(bad, False)
        bad = copy.deepcopy(good); bad.pop(key); check(bad, False)
    for key in ('candidate_driver_rpc', 'real_driver_timeout', 'ordinary_backend_bound',
                'candidate_activation', 'physical_hardware'):
        for value in (True, 1):
            bad = copy.deepcopy(good); bad[key] = value; check(bad, False)
        bad = copy.deepcopy(good); bad.pop(key); check(bad, False)

    bad = copy.deepcopy(good); bad['cases'].pop(); check(bad, False)
    bad = copy.deepcopy(good); bad['cases'].append(copy.deepcopy(bad['cases'][0])); check(bad, False)
    bad = copy.deepcopy(good); bad['cases'][1] = copy.deepcopy(bad['cases'][0]); check(bad, False)
    bad = copy.deepcopy(good); bad['cases'][0]['transport'] = 'modern'; check(bad, False)
    bad = copy.deepcopy(good); bad['cases'][0]['state'] = 'PENDING'; check(bad, False)
    bad = copy.deepcopy(good); bad['cases'][4]['mode'] = 'known-release'; check(bad, False)
    bad = copy.deepcopy(good); bad['cases'][4]['state'] = 'PENDING'; check(bad, False)

    case_flags = ('alice_denied', 'root_transport_gated', 'driver_alive', 'ram_echo_retained',
                  'disk_bytes_preserved', 'old_stop_capture_trust_preserved',
                  'known_root_reads_resumed', 'normal_discard', 'explicit_quarantine',
                  'candidate_branches_retired')
    for index, original in enumerate(good['cases']):
        for field, value in (('passed', False), ('passed', 1), ('mode', None),
                             ('state', None), ('transport', None)):
            bad = copy.deepcopy(good); bad['cases'][index][field] = value; check(bad, False)
        bad = copy.deepcopy(good); bad['cases'][index].pop('passed'); check(bad, False)
        for value in ([], [1, 2, 3], [1, True], [1.0, 2], [True, 2], [2, 1]):
            bad = copy.deepcopy(good); bad['cases'][index]['lease_serials'] = value; check(bad, False)
        bad = copy.deepcopy(good); bad['cases'][index].pop('lease_serials'); check(bad, False)
        for field in case_flags:
            expected = original[field]
            for value in (not expected, 1):
                bad = copy.deepcopy(good); bad['cases'][index][field] = value; check(bad, False)
            bad = copy.deepcopy(good); bad['cases'][index].pop(field); check(bad, False)

    for path in source_paths:
        target = fake_root / path
        original = target.read_bytes(); target.write_bytes(original + b' changed')
        check(good, False)
        target.write_bytes(original); check(good, True)
    for path in product_paths:
        target = fake_root / path
        original = target.read_bytes(); target.write_bytes(original + b' changed')
        check(good, False)
        target.write_bytes(original); check(good, True)

print('STORAGE REPLY OWNER EVIDENCE: ' + str(checks) + ' actual release-gate checks (synthetic only)')
