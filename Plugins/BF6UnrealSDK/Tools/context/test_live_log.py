"""Regression checks for MCP reading Chromium/old logs during a custom-log run."""
import ast
from pathlib import Path
import re
import tempfile
from types import SimpleNamespace
import unittest

PROJECT = next(p for p in Path(__file__).resolve().parents if (p / 'Content/Python').is_dir())
tree = ast.parse((PROJECT / 'Content/Python/bf6_context_mcp.py').read_text(encoding='utf-8'))
functions = [n for n in tree.body if isinstance(n, ast.FunctionDef)
             and n.name in {'_configured_log_path', '_live_log_path'}]


class LiveLogTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.args = ''
        fake = SimpleNamespace(
            Paths=SimpleNamespace(convert_relative_path_to_full=lambda p: p,
                                  project_log_dir=lambda: str(self.root),
                                  get_project_file_path=lambda: str(self.root / 'SDK.uproject')),
            SystemLibrary=SimpleNamespace(get_command_line=lambda: self.args))
        self.scope = {'Path': Path, 're': re, 'unreal': fake}
        exec(compile(ast.Module(body=functions, type_ignores=[]), '<production-log-functions>', 'exec'), self.scope)

    def current(self):
        return self.scope['_live_log_path']()

    def test_explicit_absolute_path_with_spaces_and_txt(self):
        log = self.root / 'test session.txt'
        log.write_text('Log file open, current session')
        self.args = '-unattended -AbsLog="' + str(log) + '"'
        self.assertEqual(self.current(), log)

    def test_missing_explicit_log_never_reads_a_different_session(self):
        (self.root / 'SDK.log').write_text('Log file open, old session')
        self.args = '-ABSLOG="' + str(self.root / 'missing.log') + '"'
        self.assertIsNone(self.current())

    def test_log_override_has_engine_precedence_over_abslog(self):
        log = self.root / 'relative name.log'
        log.write_text('Log file open, current session')
        self.args = '-ABSLOG=missing.log -LOG="relative name.log"'
        self.assertEqual(self.current(), log)

    def test_logfilename_alias(self):
        log = self.root / 'named.log'
        log.write_text('Log file open')
        self.args = '-LogFileName=named.log'
        self.assertEqual(self.current(), log)

    def test_newer_chromium_does_not_replace_project_log(self):
        log = self.root / 'SDK.log'
        log.write_text('Log file open, editor')
        (self.root / 'cef3.log').write_text('Chromium output')
        self.assertEqual(self.current(), log)

    def test_only_chromium_and_backups_is_unavailable(self):
        (self.root / 'cef3.log').write_text('Chromium output')
        (self.root / 'SDK-backup-old.log').write_text('Log file open, editor')
        self.assertIsNone(self.current())

    def test_fallback_requires_engine_log_signature(self):
        log = self.root / 'custom.log'
        log.write_text('Log file open, editor')
        (self.root / 'unrelated.log').write_text('Other application')
        self.assertEqual(self.current(), log)


if __name__ == '__main__':
    unittest.main()
