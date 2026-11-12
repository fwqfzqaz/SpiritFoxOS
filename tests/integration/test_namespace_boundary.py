"""Compile real clients and reject accidental publication of namespace state."""
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
import build


class NamespaceBoundary(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary=tempfile.TemporaryDirectory(prefix='ns-boundary-',dir=ROOT/'build')
        cls.folder=Path(cls.temporary.name).resolve()
        assert cls.folder.is_relative_to(ROOT/'build')

    @classmethod
    def tearDownClass(cls):
        assert cls.folder.resolve().is_relative_to(ROOT/'build')
        cls.temporary.cleanup()

    def compile(self,source):
        path=self.folder/(self.id().rsplit('.',1)[-1]+'.c')
        obj=path.with_suffix('.o')
        path.write_text('#include "namespace.h"\n#include "file_rpc.h"\n'+source+'\n',encoding='utf-8',newline='\n')
        output=StringIO()
        with redirect_stdout(output):
            try:
                build.run(['-target',build.HOST,'-c','-g0','-Wall','-Wextra','-Werror','-DSF_HOST_TEST','-Iinclude',str(path.relative_to(ROOT)),'-o',str(obj.relative_to(ROOT))])
            except RuntimeError:
                return False,output.getvalue()
        return obj.is_file() and obj.stat().st_size>0,output.getvalue()

    def reject_layout(self,source):
        passed,diagnostic=self.compile(source)
        self.assertFalse(passed)
        self.assertRegex(diagnostic,r'incomplete|unknown type name|never completed')

    def test_api_and_file_rpc_headers_compile(self):
        passed,diagnostic=self.compile('SfNamespace* client(void){return sf_ns_default();}')
        self.assertTrue(passed,diagnostic)

    def test_stack_storage_is_private(self):
        self.reject_layout('SfNamespace object;')

    def test_sizeof_is_private(self):
        self.reject_layout('size_t layout(void){return sizeof(SfNamespace);}')

    def test_table_members_are_private(self):
        self.reject_layout('void* table(SfNamespace*n){return n->nodes;}')

    def test_node_layout_type_is_private(self):
        self.reject_layout('SfNode object;')

    def test_handle_layout_type_is_private(self):
        self.reject_layout('SfFileHandle object;')

    def test_description_layout_type_is_private(self):
        self.reject_layout('SfDescription object;')


if __name__=='__main__':
    unittest.main(verbosity=2)
