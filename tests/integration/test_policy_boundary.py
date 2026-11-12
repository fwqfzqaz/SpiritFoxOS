"""Compile the manager policy boundaries and reject mutable console clients."""
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
import build


class PolicyBoundary(unittest.TestCase):
    def setUp(self):
        self.temporary=tempfile.TemporaryDirectory(prefix='policy-boundary-',dir=ROOT/'build')
        self.folder=Path(self.temporary.name).resolve()
        assert self.folder.is_relative_to(ROOT/'build')

    def tearDown(self):
        self.temporary.cleanup()

    def compile(self,source=None,product=None,poison=()):
        for name in poison:(self.folder/name).write_text('#error Forbidden policy dependency\n',encoding='utf-8')
        path=ROOT/product if product else self.folder/'client.c'
        if source is not None:path.write_text('#include "console_policy.h"\n'+source+'\n',encoding='utf-8')
        flags=['-target',build.HOST,'-c','-g0','-Wall','-Wextra','-Werror','-I'+str(self.folder.relative_to(ROOT)),'-Iservices','-Iinclude']
        output=StringIO()
        with redirect_stdout(output):
            try:build.run([*flags,str(path.relative_to(ROOT)),'-o',str((self.folder/'client.o').relative_to(ROOT))])
            except RuntimeError:return False,output.getvalue()
        return True,output.getvalue()

    def test_opaque_query_client(self):
        okay,diagnostic=self.compile('int identity(void){return sf_console_identity(sf_console_policy_default(),0,0,0);}')
        self.assertTrue(okay,diagnostic)

    def test_client_cannot_allocate_console_state(self):
        okay,diagnostic=self.compile('SfConsolePolicy policy;')
        self.assertFalse(okay);self.assertRegex(diagnostic,r'incomplete|never completed|tentative definition')

    def test_session_view_is_readonly(self):
        okay,diagnostic=self.compile('void change(SfConsolePolicy*p){sf_console_sessions(p)->active=1;}')
        self.assertFalse(okay);self.assertRegex(diagnostic,r'const|read-only')

    def test_account_view_is_readonly(self):
        okay,diagnostic=self.compile('void change(SfConsolePolicy*p){sf_console_accounts(p)->accounts[0].uid=0;}')
        self.assertFalse(okay);self.assertRegex(diagnostic,r'const|read-only')

    def test_fixture_not_exposed_to_runtime(self):
        okay,diagnostic=self.compile('int change(SfConsolePolicy*p){return sf_console_policy_fixture(p,"",0);}')
        self.assertFalse(okay);self.assertRegex(diagnostic,r'undeclared|implicit')

    def test_launch_policy_has_no_account_or_namespace_dependency(self):
        okay,diagnostic=self.compile(product='services/launch_policy.c',poison=('accounts.h','session.h','namespace.h','console_policy.h'))
        self.assertTrue(okay,diagnostic)

    def test_console_policy_has_no_loader_or_namespace_dependency(self):
        okay,diagnostic=self.compile(product='services/console_policy.c',poison=('package.h','native_image.h','namespace.h','launch_policy.h'))
        self.assertTrue(okay,diagnostic)


if __name__=='__main__':unittest.main(verbosity=2)
