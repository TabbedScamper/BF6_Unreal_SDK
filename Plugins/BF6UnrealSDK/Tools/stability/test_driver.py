"""Exercise driver startup and distinct-frame sampling without launching Unreal."""
import json
import os
from pathlib import Path
import runpy
import sys
import tempfile
from types import SimpleNamespace as NS
import unittest
from unittest.mock import patch

class Driver(unittest.TestCase):
    def test_startup_and_duplicate_slate_ticks(self):
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory, 'config.json')
            config.write_text(json.dumps(dict(output=directory, flight_seconds=30, flight_radius_m=0)))
            frame = [100]
            lifecycle = []
            vector = lambda x, y, z: NS(x=x, y=y, z=z)
            unreal = NS(EditorPythonScripting=NS(set_keep_python_script_alive=lambda value: lifecycle.append(('alive', value))),
                SystemLibrary=NS(get_frame_count=lambda: frame[0], quit_editor=lambda: lifecycle.append(('quit', True))),
                get_editor_subsystem=lambda _: None, EditorActorSubsystem=object,
                EditorLoadingAndSavingUtils=NS(), register_slate_post_tick_callback=lambda _: 1,
                unregister_slate_post_tick_callback=lambda handle: lifecycle.append(('unregister', handle)),
                log=lambda _: None, Vector=vector, Rotator=lambda **kw: NS(**kw))
            with patch.dict(sys.modules, unreal=unreal), patch.dict(os.environ, BF6_STABILITY_CONFIG=str(config)):
                module = runpy.run_path(str(Path(__file__).with_name('editor_workflow.py')))
                self.assertIn('WORKFLOW', module, Path(directory, 'workflow.json').read_text() if Path(directory, 'workflow.json').exists() else '')
                w = module['WORKFLOW']
                self.assertEqual(w.state, 'boot')
                w.state = 'flight'; w.next_at = 0; w.flight_at = 0; w.last = 0
                w.last_engine_time = 0; w.last_engine_frame = 100
                w.location = vector(0, 0, 0); w.rotation = NS(pitch=0, yaw=0, roll=0)
                moves = []
                w.set_camera = lambda p, r: moves.append((p, r))
                for number, seconds in ((100, .001), (101, .02), (101, .021), (102, .04)):
                    frame[0] = number
                    with patch.object(module['time'], 'perf_counter', return_value=seconds):
                        w.tick(0)
                self.assertEqual(len(w.frames), 4)
                self.assertEqual(w.engine_frames, [20, 20])
                self.assertEqual(len(moves), 2)
                # A deferred memory report must see the populated scene, not
                # the scratch world created by the later persistence checks.
                w.state = 'memory_report'; w.memory_dir = Path(directory, 'MemReports')
                w.memory_dir.mkdir(); w.memory_before = set(); w.memory_at = 0
                finishes = []; w.finish_scene = lambda: finishes.append(True)
                with patch.object(module['time'], 'perf_counter', return_value=.05): w.tick(0)
                self.assertEqual(finishes, [])
                nested = w.memory_dir / 'populated'; nested.mkdir()
                (nested / 'scene.memreport').write_text('scene resources')
                with patch.object(module['time'], 'perf_counter', return_value=.06): w.tick(0)
                self.assertEqual(finishes, [True])
                # No queued compiler jobs is insufficient if the active-quality
                # material shader maps were never populated.
                w.config.update(level='MP_Dumbo', stage_timeout=60, min_placed=0)
                w.state = 'ready'; w.next_at = 0; w.open_at = 0; w.ready_frame = None
                state = dict(sdk=dict(level='MP_Dumbo', actors=1, placed=0, compiling=0),
                             highpoly=dict(completed=True, builtAnything=True, building=False,
                                           coreBusy=False, previewsBusy=False, resolving='',
                                           incompleteParentMaterials=['M_Test']))
                w.snapshot = lambda: state
                started = []; w.start_flight = lambda _: started.append(True)
                with patch.object(module['time'], 'perf_counter', return_value=.1): w.tick(0)
                self.assertEqual(started, [])
                self.assertEqual(w.result['opens'], [])
                state['highpoly']['incompleteParentMaterials'].clear()
                with patch.object(module['time'], 'perf_counter', return_value=1): w.tick(0)
                self.assertEqual(started, [])
                frame[0] = 104
                with patch.object(module['time'], 'perf_counter', return_value=1.1): w.tick(0)
                self.assertEqual(started, [True])
                frame[0] = 102
                w.state = 'configured'; w.next_at = 0; w.configured_frame = 102
                opened = []; w.begin_open = lambda: opened.append(True)
                with patch.object(module['time'], 'perf_counter', return_value=2): w.tick(0)
                self.assertEqual(opened, [])
                frame[0] = 103
                with patch.object(module['time'], 'perf_counter', return_value=3): w.tick(0)
                self.assertEqual(opened, [])
                frame[0] = 104
                with patch.object(module['time'], 'perf_counter', return_value=4): w.tick(0)
                self.assertEqual(opened, [True])
                # Unreal chooses UTF-16 for exports containing creator names
                # outside ASCII. Both export encodings must reach reopen checks.
                w.config.update(save='Night Ops', mode='high')
                w.levels = NS(load_map=lambda _: True)
                w.edit_save_reopen = lambda: None
                finishes = []
                w.finish = lambda: finishes.append(True)
                for encoding in ('utf-8-sig', 'utf-16', 'utf-16-be'):
                    document = json.dumps({'mapRotation': ['MP_Isolated'], 'name': 'Créateur'}, ensure_ascii=False)
                    raw = document.encode(encoding)
                    if encoding == 'utf-16-be': raw = b'\xfe\xff' + raw
                    w.command = lambda _, payload=raw: Path(directory, 'experience-export.json').write_bytes(payload)
                    module['Workflow'].finish_scene(w)
                self.assertEqual(finishes, [True, True, True])
                module['Workflow'].finish(w)
                self.assertEqual(lifecycle, [('alive', True), ('unregister', 1), ('alive', False)])
                self.assertTrue(json.loads(Path(directory, 'workflow.json').read_text())['completed'])

if __name__ == '__main__': unittest.main(verbosity=2)
