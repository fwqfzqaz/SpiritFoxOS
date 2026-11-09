"""Adapt a Rust Windows GNU staticlib to Zig's compiler-rt ownership."""
import struct


def stack_probe_member(data):
    """Find the archive member defining only the overlapping stack probes.

    Read the GNU/COFF archive's first linker index rather than depending on
    rustc's hashed object name or codegen-unit number. Leave all other Rust
    builtins intact. Unexpected formats or combined objects fail closed.
    """
    if not data.startswith(b'!<arch>\n'):
        raise ValueError('Rust library is not a regular ar archive')
    members = {}
    pos = 8
    while pos < len(data):
        header = data[pos:pos + 60]
        if len(header) != 60 or header[58:] != b'`\n':
            raise ValueError('Invalid Rust archive member header')
        size = int(header[48:58])
        end = pos + 60 + size
        if end > len(data):
            raise ValueError('Truncated Rust archive member')
        members[pos] = (header[:16].rstrip(), data[pos + 60:end])
        pos = end + (size & 1)
    name, index = members[8]
    if name != b'/' or len(index) < 4:
        raise ValueError('Missing GNU/COFF archive linker index')
    count = struct.unpack_from('>I', index)[0]
    if 4 + 4 * count > len(index):
        raise ValueError('Truncated archive linker index')
    offsets = struct.unpack_from('>' + str(count) + 'I', index, 4)
    names = index[4 + 4 * count:].split(b'\0')
    if len(names) <= count or any(not n for n in names[:count]):
        raise ValueError('Invalid archive symbol names')
    symbols = list(zip(offsets, names[:count]))
    probes = {b'___chkstk_ms', b'___chkstk', b'__chkstk'}
    selected = {offset for offset, symbol in symbols if symbol in probes}
    if not selected:
        return None
    if len(selected) != 1:
        raise ValueError('Rust stack probes span multiple archive members')
    offset = selected.pop()
    if any(symbol not in probes for at, symbol in symbols if at == offset):
        raise ValueError('Rust stack probe member also defines other symbols')
    name, _ = members[offset]
    if name.startswith(b'/') and name[1:].isdigit():
        strings = next(body for key, body in members.values() if key == b'//')
        name = strings[int(name[1:]):].split(b'/\n', 1)[0].split(b'\0', 1)[0]
    else:
        name = name.rstrip(b'/')
    result = name.decode('ascii')
    if not result.startswith('compiler_builtins-') or not result.endswith('.o'):
        raise ValueError('Stack probe is not an isolated Rust compiler_builtins object')
    return result
