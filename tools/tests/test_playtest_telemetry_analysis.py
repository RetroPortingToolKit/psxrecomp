"""Coverage claims must distinguish missing probes, inactive checks and activity."""
import csv
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'replay_diagnostics.py'
spec = importlib.util.spec_from_file_location('replay', SCRIPT)
replay = importlib.util.module_from_spec(spec)
spec.loader.exec_module(replay)


class Coverage(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)

    def write(self, rows, version='v2'):
        (self.folder / 'session.json').write_text(json.dumps({'telemetry_format': 'psxrecomp-telemetry-' + version}))
        with (self.folder / 'telemetry.csv').open('w', newline='') as output:
            writer = csv.DictWriter(output, fieldnames=rows[0].keys())
            writer.writeheader()
            writer.writerows(rows)

    def samples(self, modern=False):
        rows = [dict(sequence=i, host_elapsed_us=i*16667, host_interval_us=16667,
                     render_verify_checks=0, render_verify_mismatch=0) for i in range(3)]
        if modern:
            for i, row in enumerate(rows):
                row.update(gpu_draw_commands=200*i, host_presents=i, host_present_errors=0,
                           host_present_status_available=1,
                           video_snapshot_valid=1, video_renderer=2,
                           video_resolution_preset=1 if i == 0 else 1080,
                           video_aspect_num=4 if i == 0 else 16,
                           video_aspect_den=3 if i == 0 else 9, video_widescreen=int(i > 0),
                           render_verify_enabled=0, visual_verify_available=0, visual_verify_checks=0)
                row.update(composed_renderer=2, composed_internal_scale=1 if i == 0 else 5,
                           composed_source_width=320 if i == 0 else 426, composed_source_height=240,
                           composed_drawable_width=640 if i == 0 else 1920,
                           composed_drawable_height=480 if i == 0 else 1080,
                           composed_viewport_width=640 if i == 0 else 1920,
                           composed_viewport_height=480 if i == 0 else 1080)
        return rows

    def test_legacy_zero_checks_are_unknown_coverage_not_success(self):
        self.write(self.samples(), 'v1')
        result = replay.analyze_telemetry(self.folder)
        self.assertFalse(result['ordinary_draw_commands']['available'])
        self.assertFalse(result['render_state_verification']['covered_in_observed_interval'])
        self.assertIsNone(result['render_state_verification']['enabled'])
        self.assertFalse(result['visual_verification']['available'])
        self.assertTrue(any('not recorded' in text for text in result['warnings']))

    def test_normal_rendering_can_advance_with_no_extra_render_passes(self):
        self.write(self.samples(True))
        result = replay.analyze_telemetry(self.folder)
        self.assertEqual(result['ordinary_draw_commands']['observed_increase'], 400)
        self.assertEqual(result['host_present_calls']['observed_increase'], 2)
        self.assertFalse(result['render_state_verification']['covered_in_observed_interval'])
        self.assertEqual(result['visual_verification']['result'], 'not evaluated')
        self.assertEqual(result['effective_video_mode_count'], 2)
        self.assertEqual(result['effective_video_modes'][1]['video_resolution_preset'], 1080)
        self.assertEqual(result['composed_video_modes'][1]['composed_internal_scale'], 5)

    def test_enabled_but_never_exercised_and_preexisting_checks_are_uncovered(self):
        rows = self.samples(True)
        for row in rows:
            row['render_verify_enabled'] = 1
            row['render_verify_checks'] = 9
        self.write(rows)
        result = replay.analyze_telemetry(self.folder)['render_state_verification']
        self.assertTrue(result['enabled'])
        self.assertFalse(result['covered_in_observed_interval'])

    def test_checks_do_not_certify_pixels_and_counter_resets_remain_visible(self):
        rows = self.samples(True)
        for row, checks in zip(rows, (0, 3, 1)):
            row['render_verify_checks'] = checks
            row['render_verify_enabled'] = 1
        rows[-1]['host_present_errors'] = 1
        self.write(rows)
        result = replay.analyze_telemetry(self.folder)
        self.assertTrue(result['render_state_verification']['covered_in_observed_interval'])
        self.assertEqual(result['render_state_verification']['checks']['resets'], 1)
        self.assertEqual(result['host_present_errors']['observed_increase'], 1)
        self.assertFalse(result['visual_verification']['available'])

    def test_torn_final_record_is_ignored_and_internal_malformed_rows_fail(self):
        self.write(self.samples(True))
        path = self.folder / 'telemetry.csv'
        with path.open('a') as output:
            output.write('3,partial')
        self.assertTrue(replay.analyze_telemetry(self.folder)['ignored_torn_final_row'])
        with path.open('a') as output:
            output.write('\n')
        with self.assertRaises(ValueError):
            replay.analyze_telemetry(self.folder)

    def test_analysis_cli_needs_no_build_disc_or_replay_output(self):
        self.write(self.samples(True))
        completed = subprocess.run([sys.executable, '-I', str(SCRIPT), str(self.folder), '--analyze'],
                                   check=True, capture_output=True, text=True)
        self.assertEqual(json.loads(completed.stdout)['samples'], 3)
        self.assertFalse((self.folder / 'replay').exists())

    def test_incident_request_acceptance_does_not_invent_a_written_image(self):
        rows = self.samples(True)
        for row, state in zip(rows, (-1, 1, 0)):
            row['incident_capture_request'] = state
        self.write(rows)
        result = replay.analyze_telemetry(self.folder)['incident_captures']
        self.assertEqual(result['accepted_requests'], 1)
        self.assertEqual(result['rejected_requests'], 1)
        self.assertEqual(result['files_present'], [])
        (self.folder / 'incident-1.png').write_bytes(b'fixture')
        self.assertEqual(replay.analyze_telemetry(self.folder)['incident_captures']['files_present'], ['incident-1.png'])

    def test_sdl2_void_present_is_activity_with_unknown_failure_coverage(self):
        rows = self.samples(True)
        for row in rows:
            row['host_present_status_available'] = 0
        self.write(rows)
        result = replay.analyze_telemetry(self.folder)
        self.assertEqual(result['host_present_calls']['observed_increase'], 2)
        self.assertFalse(result['host_present_status_available_all_observed_calls'])
        self.assertTrue(any('SDL2' in warning for warning in result['warnings']))

    def test_startup_before_first_present_is_not_a_missing_result_for_a_call(self):
        rows = self.samples(True)
        rows[0]['host_present_status_available'] = 0
        self.write(rows)
        self.assertTrue(replay.analyze_telemetry(self.folder)['host_present_status_available_all_observed_calls'])


if __name__ == '__main__':
    unittest.main()
