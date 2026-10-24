"""Windows exports must be present before the first use of an overlay function."""
import sys
import unittest
from unittest.mock import patch
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import compile_overlays


class ExportDeclarationTests(unittest.TestCase):
    def test_forward_declarations_and_definitions_share_export(self):
        source = '''void func_80010000(CPUState* cpu);
void func_80010004(CPUState* cpu);
void func_80020000(CPUState* cpu);
void func_80010000(CPUState* cpu)
{ func_80010004(cpu); func_80020000(cpu); }
void func_80010004(CPUState* cpu)
{}
'''
        with patch.object(compile_overlays, 'DISPATCH_PREAMBLE', ''):
            patched = compile_overlays.patch_generated_c(source, 0x80010000, 8)
        export = '__attribute__((visibility("default")))\n#endif\n'
        for address in ('80010000', '80010004'):
            signature = f'void func_{address}(CPUState* cpu)'
            self.assertIn(export + signature + ';', patched)
            self.assertIn(export + signature + '\n', patched)
        self.assertNotIn('void func_80020000', patched)
        self.assertIn('call_by_address(cpu, 0x80020000u)', patched)


if __name__ == '__main__':
    unittest.main()
